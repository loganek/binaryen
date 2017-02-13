/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Optimize combinations of instructions
//

#include <algorithm>

#include <wasm.h>
#include <pass.h>
#include <wasm-s-parser.h>
#include <support/threads.h>
#include <ast_utils.h>
#include <ast/cost.h>
#include <ast/properties.h>

namespace wasm {

Name I32_EXPR  = "i32.expr",
     I64_EXPR  = "i64.expr",
     F32_EXPR  = "f32.expr",
     F64_EXPR  = "f64.expr",
     ANY_EXPR  = "any.expr";

// A pattern
struct Pattern {
  Expression* input;
  Expression* output;

  Pattern(Expression* input, Expression* output) : input(input), output(output) {}
};

#if 0
// Database of patterns
struct PatternDatabase {
  Module wasm;

  char* input;

  std::map<Expression::Id, std::vector<Pattern>> patternMap; // root expression id => list of all patterns for it TODO optimize more

  PatternDatabase() {
    // generate module
    input = strdup(
      #include "OptimizeInstructions.wast.processed"
    );
    try {
      SExpressionParser parser(input);
      Element& root = *parser.root;
      SExpressionWasmBuilder builder(wasm, *root[0]);
      // parse module form
      auto* func = wasm.getFunction("patterns");
      auto* body = func->body->cast<Block>();
      for (auto* item : body->list) {
        auto* pair = item->cast<Block>();
        patternMap[pair->list[0]->_id].emplace_back(pair->list[0], pair->list[1]);
      }
    } catch (ParseException& p) {
      p.dump(std::cerr);
      Fatal() << "error in parsing wasm binary";
    }
  }

  ~PatternDatabase() {
    free(input);
  };
};

static PatternDatabase* database = nullptr;

struct DatabaseEnsurer {
  DatabaseEnsurer() {
    assert(!database);
    database = new PatternDatabase;
  }
};
#endif

// Check for matches and apply them
struct Match {
  Module& wasm;
  Pattern& pattern;

  Match(Module& wasm, Pattern& pattern) : wasm(wasm), pattern(pattern) {}

  std::vector<Expression*> wildcards; // id in i32.any(id) etc. => the expression it represents in this match

  // Comparing/checking

  // Check if we can match to this pattern, updating ourselves with the info if so
  bool check(Expression* seen) {
    // compare seen to the pattern input, doing a special operation for our "wildcards"
    assert(wildcards.size() == 0);
    auto compare = [this](Expression* subInput, Expression* subSeen) {
      CallImport* call = subInput->dynCast<CallImport>();
      if (!call || call->operands.size() != 1 || call->operands[0]->type != i32 || !call->operands[0]->is<Const>()) return false;
      Index index = call->operands[0]->cast<Const>()->value.geti32();
      // handle our special functions
      auto checkMatch = [&](WasmType type) {
        if (type != none && subSeen->type != type) return false;
        while (index >= wildcards.size()) {
          wildcards.push_back(nullptr);
        }
        if (!wildcards[index]) {
          // new wildcard
          wildcards[index] = subSeen; // NB: no need to copy
          return true;
        } else {
          // We are seeing this index for a second or later time, check it matches
          return ExpressionAnalyzer::equal(subSeen, wildcards[index]);
        };
      };
      if (call->target == I32_EXPR) {
        if (checkMatch(i32)) return true;
      } else if (call->target == I64_EXPR) {
        if (checkMatch(i64)) return true;
      } else if (call->target == F32_EXPR) {
        if (checkMatch(f32)) return true;
      } else if (call->target == F64_EXPR) {
        if (checkMatch(f64)) return true;
      } else if (call->target == ANY_EXPR) {
        if (checkMatch(none)) return true;
      }
      return false;
    };

    return ExpressionAnalyzer::flexibleEqual(pattern.input, seen, compare);
  }


  // Applying/copying

  // Apply the match, generate an output expression from the matched input, performing substitutions as necessary
  Expression* apply() {
    // When copying a wildcard, perform the substitution.
    // TODO: we can reuse nodes, not copying a wildcard when it appears just once, and we can reuse other individual nodes when they are discarded anyhow.
    auto copy = [this](Expression* curr) -> Expression* {
      CallImport* call = curr->dynCast<CallImport>();
      if (!call || call->operands.size() != 1 || call->operands[0]->type != i32 || !call->operands[0]->is<Const>()) return nullptr;
      Index index = call->operands[0]->cast<Const>()->value.geti32();
      // handle our special functions
      if (call->target == I32_EXPR || call->target == I64_EXPR || call->target == F32_EXPR || call->target == F64_EXPR || call->target == ANY_EXPR) {
        return ExpressionManipulator::copy(wildcards.at(index), wasm);
      }
      return nullptr;
    };
    return ExpressionManipulator::flexibleCopy(pattern.output, wasm, copy);
  }
};

// Utilities

// returns the maximum amount of bits used in an integer expression
// not extremely precise (doesn't look into add operands, etc.)
static Index getMaxBits(Expression* curr) {
  if (auto* const_ = curr->dynCast<Const>()) {
    switch (curr->type) {
      case i32: return 32 - const_->value.countLeadingZeroes().geti32();
      case i64: return 64 - const_->value.countLeadingZeroes().geti64();
      default: WASM_UNREACHABLE();
    }
  } else if (auto* binary = curr->dynCast<Binary>()) {
    switch (binary->op) {
      // 32-bit
      case AddInt32: case SubInt32: case MulInt32:
      case DivSInt32: case DivUInt32: case RemSInt32:
      case RemUInt32: case RotLInt32: case RotRInt32: return 32;
      case AndInt32: case XorInt32: return std::min(getMaxBits(binary->left), getMaxBits(binary->right));
      case OrInt32: return std::max(getMaxBits(binary->left), getMaxBits(binary->right));
      case ShlInt32: {
        if (auto* shifts = binary->right->dynCast<Const>()) {
          return std::min(Index(32), getMaxBits(binary->left) + shifts->value.geti32());
        }
        return 32;
      }
      case ShrUInt32: {
        if (auto* shift = binary->right->dynCast<Const>()) {
          auto maxBits = getMaxBits(binary->left);
          auto shifts = std::min(Index(shift->value.geti32()), maxBits); // can ignore more shifts than zero us out
          return std::max(Index(0), maxBits - shifts);
        }
        return 32;
      }
      case ShrSInt32: {
        if (auto* shift = binary->right->dynCast<Const>()) {
          auto maxBits = getMaxBits(binary->left);
          if (maxBits == 32) return 32;
          auto shifts = std::min(Index(shift->value.geti32()), maxBits); // can ignore more shifts than zero us out
          return std::max(Index(0), maxBits - shifts);
        }
        return 32;
      }
      // 64-bit TODO
      // comparisons
      case EqInt32: case NeInt32: case LtSInt32:
      case LtUInt32: case LeSInt32: case LeUInt32:
      case GtSInt32: case GtUInt32: case GeSInt32:
      case GeUInt32:
      case EqInt64: case NeInt64: case LtSInt64:
      case LtUInt64: case LeSInt64: case LeUInt64:
      case GtSInt64: case GtUInt64: case GeSInt64:
      case GeUInt64:
      case EqFloat32: case NeFloat32:
      case LtFloat32: case LeFloat32: case GtFloat32: case GeFloat32:
      case EqFloat64: case NeFloat64:
      case LtFloat64: case LeFloat64: case GtFloat64: case GeFloat64: return 1;
      default: {}
    }
  } else if (auto* unary = curr->dynCast<Unary>()) {
    switch (unary->op) {
      case ClzInt32: case CtzInt32: case PopcntInt32: return 5;
      case ClzInt64: case CtzInt64: case PopcntInt64: return 6;
      case EqZInt32: case EqZInt64: return 1;
      case WrapInt64: return std::min(Index(32), getMaxBits(unary->value));
      default: {}
    }
  } else if (auto* set = curr->dynCast<SetLocal>()) {
    // a tee passes through the value
    return getMaxBits(set->value);
  } else if (auto* load = curr->dynCast<Load>()) {
    return 8 * load->bytes;
  }
  switch (curr->type) {
    case i32: return 32;
    case i64: return 64;
    case unreachable: return 64; // not interesting, but don't crash
    default: WASM_UNREACHABLE();
  }
}

// Check if an expression is a sign-extend, and if so, returns the value
// that is extended, otherwise nullptr
static Expression* getSignExt(Expression* curr) {
  if (auto* outer = curr->dynCast<Binary>()) {
    if (outer->op == ShrSInt32) {
      if (auto* outerConst = outer->right->dynCast<Const>()) {
        if (auto* inner = outer->left->dynCast<Binary>()) {
          if (inner->op == ShlInt32) {
            if (auto* innerConst = inner->right->dynCast<Const>()) {
              if (outerConst->value == innerConst->value) {
                return inner->left;
              }
            }
          }
        }
      }
    }
  }
  return nullptr;
}

// gets the size of the sign-extended value
static Index getSignExtBits(Expression* curr) {
  return 32 - curr->cast<Binary>()->right->cast<Const>()->value.geti32();
}

// Check if an expression is almost a sign-extend: perhaps the inner shift
// is too large. We can split the shifts in that case, which is sometimes
// useful (e.g. if we can remove the signext)
static Expression* getAlmostSignExt(Expression* curr) {
  if (auto* outer = curr->dynCast<Binary>()) {
    if (outer->op == ShrSInt32) {
      if (auto* outerConst = outer->right->dynCast<Const>()) {
        if (auto* inner = outer->left->dynCast<Binary>()) {
          if (inner->op == ShlInt32) {
            if (auto* innerConst = inner->right->dynCast<Const>()) {
              if (outerConst->value.leU(innerConst->value).geti32()) {
                return inner->left;
              }
            }
          }
        }
      }
    }
  }
  return nullptr;
}

// gets the size of the almost sign-extended value, as well as the
// extra shifts, if any
static Index getAlmostSignExtBits(Expression* curr, Index& extraShifts) {
  extraShifts = curr->cast<Binary>()->left->cast<Binary>()->right->cast<Const>()->value.geti32() -
                curr->cast<Binary>()->right->cast<Const>()->value.geti32();
  return getSignExtBits(curr);
}

// get a mask to keep only the low # of bits
static int32_t lowBitMask(int32_t bits) {
  uint32_t ret = -1;
  if (bits >= 32) return ret;
  return ret >> (32 - bits);
}

// Main pass class
struct OptimizeInstructions : public WalkerPass<PostWalker<OptimizeInstructions, UnifiedExpressionVisitor<OptimizeInstructions>>> {
  bool isFunctionParallel() override { return true; }

  Pass* create() override { return new OptimizeInstructions; }

  void prepareToRun(PassRunner* runner, Module* module) override {
#if 0
    static DatabaseEnsurer ensurer;
#endif
  }

  void visitExpression(Expression* curr) {
    // we may be able to apply multiple patterns, one may open opportunities that look deeper NB: patterns must not have cycles
    while (1) {
      auto* handOptimized = handOptimize(curr);
      if (handOptimized) {
        curr = handOptimized;
        replaceCurrent(curr);
        continue;
      }
#if 0
      auto iter = database->patternMap.find(curr->_id);
      if (iter == database->patternMap.end()) return;
      auto& patterns = iter->second;
      bool more = false;
      for (auto& pattern : patterns) {
        Match match(*getModule(), pattern);
        if (match.check(curr)) {
          curr = match.apply();
          replaceCurrent(curr);
          more = true;
          break; // exit pattern for loop, return to main while loop
        }
      }
      if (!more) break;
#else
      break;
#endif
    }
  }

  // Optimizations that don't yet fit in the pattern DSL, but could be eventually maybe
  Expression* handOptimize(Expression* curr) {
    if (auto* binary = curr->dynCast<Binary>()) {
      if (Properties::isSymmetric(binary)) {
        // canonicalize a const to the second position
        if (binary->left->is<Const>() && !binary->right->is<Const>()) {
          std::swap(binary->left, binary->right);
        }
      }
      if (auto* ext = getAlmostSignExt(binary)) {
        Index extraShifts;
        auto bits = getAlmostSignExtBits(binary, extraShifts);
        auto* load = ext->dynCast<Load>();
        // pattern match a load of 8 bits and a sign extend using a shl of 24 then shr_s of 24 as well, etc.
        if (load && ((load->bytes == 1 && bits == 8) || (load->bytes == 2 && bits == 16))) {
          load->signed_ = true;
          return removeAlmostSignExt(binary);
        }
        // if the sign-extend input cannot have a sign bit, we don't need it
        if (getMaxBits(ext) + extraShifts < bits) {
          return removeAlmostSignExt(binary);
        }
      } else if (binary->op == EqInt32 || binary->op == NeInt32) {
        if (auto* c = binary->right->dynCast<Const>()) {
          if (auto* ext = getSignExt(binary->left)) {
            // we are comparing a sign extend to a constant, which means we can use a cheaper zext
            auto bits = getSignExtBits(binary->left);
            binary->left = makeZeroExt(ext, bits);
            // the const we compare to only needs the relevant bits
            c->value = c->value.and_(Literal(lowBitMask(bits)));
            return binary;
          }
          if (binary->op == EqInt32 && c->value.geti32() == 0) {
            // equal 0 => eqz
            return Builder(*getModule()).makeUnary(EqZInt32, binary->left);
          }
        } else if (auto* left = getSignExt(binary->left)) {
          if (auto* right = getSignExt(binary->right)) {
            // we are comparing two sign-exts, so we may as well replace both with cheaper zexts
            auto bits = getSignExtBits(binary->left);
            binary->left = makeZeroExt(left, bits);
            binary->right = makeZeroExt(right, bits);
            return binary;
          }
        }
        // note that both left and right may be consts, but then we let precompute compute the constant result
      } else if (binary->op == AddInt32 || binary->op == SubInt32) {
        return optimizeAddedConstants(binary);
      }
      // a bunch of operations on a constant right side can be simplified
      if (auto* right = binary->right->dynCast<Const>()) {
        if (binary->op == AndInt32) {
          auto mask = right->value.geti32();
          // and with -1 does nothing (common in asm.js output)
          if (mask == -1) {
            return binary->left;
          }
          // small loads do not need to be masted, the load itself masks
          if (auto* load = binary->left->dynCast<Load>()) {
            if ((load->bytes == 1 && mask == 0xff) ||
                (load->bytes == 2 && mask == 0xffff)) {
              load->signed_ = false;
              return load;
            }
          } else if (mask == 1 && Properties::emitsBoolean(binary->left)) {
            // (bool) & 1 does not need the outer mask
            return binary->left;
          }
        }
        // the square of some operations can be merged
        if (auto* left = binary->left->dynCast<Binary>()) {
          if (left->op == binary->op) {
            if (auto* leftRight = left->right->dynCast<Const>()) {
              if (left->op == AndInt32) {
                leftRight->value = leftRight->value.and_(right->value);
                return left;
              } else if (left->op == OrInt32) {
                leftRight->value = leftRight->value.or_(right->value);
                return left;
              } else if (left->op == ShlInt32 || left->op == ShrUInt32 || left->op == ShrSInt32) {
                leftRight->value = leftRight->value.add(right->value);
                return left;
              }
            }
          }
        }
      }
      if (binary->op == AndInt32 || binary->op == OrInt32) {
        return conditionalizeExpensiveOnBitwise(binary);
      }
    } else if (auto* unary = curr->dynCast<Unary>()) {
      // de-morgan's laws
      if (unary->op == EqZInt32) {
        if (auto* inner = unary->value->dynCast<Binary>()) {
          switch (inner->op) {
            case EqInt32:  inner->op = NeInt32;  return inner;
            case NeInt32:  inner->op = EqInt32;  return inner;
            case LtSInt32: inner->op = GeSInt32; return inner;
            case LtUInt32: inner->op = GeUInt32; return inner;
            case LeSInt32: inner->op = GtSInt32; return inner;
            case LeUInt32: inner->op = GtUInt32; return inner;
            case GtSInt32: inner->op = LeSInt32; return inner;
            case GtUInt32: inner->op = LeUInt32; return inner;
            case GeSInt32: inner->op = LtSInt32; return inner;
            case GeUInt32: inner->op = LtUInt32; return inner;

            case EqInt64:  inner->op = NeInt64;  return inner;
            case NeInt64:  inner->op = EqInt64;  return inner;
            case LtSInt64: inner->op = GeSInt64; return inner;
            case LtUInt64: inner->op = GeUInt64; return inner;
            case LeSInt64: inner->op = GtSInt64; return inner;
            case LeUInt64: inner->op = GtUInt64; return inner;
            case GtSInt64: inner->op = LeSInt64; return inner;
            case GtUInt64: inner->op = LeUInt64; return inner;
            case GeSInt64: inner->op = LtSInt64; return inner;
            case GeUInt64: inner->op = LtUInt64; return inner;

            case EqFloat32: inner->op = NeFloat32; return inner;
            case NeFloat32: inner->op = EqFloat32; return inner;

            case EqFloat64: inner->op = NeFloat64; return inner;
            case NeFloat64: inner->op = EqFloat64; return inner;

            default: {}
          }
        }
        // eqz of a sign extension can be of zero-extension
        if (auto* ext = getSignExt(unary->value)) {
          // we are comparing a sign extend to a constant, which means we can use a cheaper zext
          auto bits = getSignExtBits(unary->value);
          unary->value = makeZeroExt(ext, bits);
          return unary;
        }
      }
    } else if (auto* set = curr->dynCast<SetGlobal>()) {
      // optimize out a set of a get
      auto* get = set->value->dynCast<GetGlobal>();
      if (get && get->name == set->name) {
        ExpressionManipulator::nop(curr);
      }
    } else if (auto* iff = curr->dynCast<If>()) {
      iff->condition = optimizeBoolean(iff->condition);
      if (iff->ifFalse) {
        if (auto* unary = iff->condition->dynCast<Unary>()) {
          if (unary->op == EqZInt32) {
            // flip if-else arms to get rid of an eqz
            iff->condition = unary->value;
            std::swap(iff->ifTrue, iff->ifFalse);
          }
        }
      }
    } else if (auto* select = curr->dynCast<Select>()) {
      select->condition = optimizeBoolean(select->condition);
      auto* condition = select->condition->dynCast<Unary>();
      if (condition && condition->op == EqZInt32) {
        // flip select to remove eqz, if we can reorder
        EffectAnalyzer ifTrue(getPassOptions(), select->ifTrue);
        EffectAnalyzer ifFalse(getPassOptions(), select->ifFalse);
        if (!ifTrue.invalidates(ifFalse)) {
          select->condition = condition->value;
          std::swap(select->ifTrue, select->ifFalse);
        }
      }
    } else if (auto* br = curr->dynCast<Break>()) {
      if (br->condition) {
        br->condition = optimizeBoolean(br->condition);
      }
    } else if (auto* load = curr->dynCast<Load>()) {
      optimizeMemoryAccess(load->ptr, load->offset);
    } else if (auto* store = curr->dynCast<Store>()) {
      optimizeMemoryAccess(store->ptr, store->offset);
      // stores of fewer bits truncates anyhow
      if (auto* binary = store->value->dynCast<Binary>()) {
        if (binary->op == AndInt32) {
          if (auto* right = binary->right->dynCast<Const>()) {
            if (right->type == i32) {
              auto mask = right->value.geti32();
              if ((store->bytes == 1 && mask == 0xff) ||
                  (store->bytes == 2 && mask == 0xffff)) {
                store->value = binary->left;
              }
            }
          }
        } else if (auto* ext = getSignExt(binary)) {
          // if sign extending the exact bit size we store, we can skip the extension
          // if extending something bigger, then we just alter bits we don't save anyhow
          if (getSignExtBits(binary) >= store->bytes * 8) {
            store->value = ext;
          }
        }
      } else if (auto* unary = store->value->dynCast<Unary>()) {
        if (unary->op == WrapInt64) {
          // instead of wrapping to 32, just store some of the bits in the i64
          store->valueType = i64;
          store->value = unary->value;
        }
      }
    }
    return nullptr;
  }

private:
  // Optimize given that the expression is flowing into a boolean context
  Expression* optimizeBoolean(Expression* boolean) {
    if (auto* unary = boolean->dynCast<Unary>()) {
      if (unary && unary->op == EqZInt32) {
        auto* unary2 = unary->value->dynCast<Unary>();
        if (unary2 && unary2->op == EqZInt32) {
          // double eqz
          return unary2->value;
        }
      }
    } else if (auto* binary = boolean->dynCast<Binary>()) {
      if (binary->op == OrInt32) {
        // an or flowing into a boolean context can consider each input as boolean
        binary->left = optimizeBoolean(binary->left);
        binary->right = optimizeBoolean(binary->right);
      } else if (binary->op == NeInt32) {
        // x != 0 is just x if it's used as a bool
        if (auto* num = binary->right->dynCast<Const>()) {
          if (num->value.geti32() == 0) {
            return binary->left;
          }
        }
      }
      if (auto* ext = getSignExt(binary)) {
        // use a cheaper zero-extent, we just care about the boolean value anyhow
        return makeZeroExt(ext, getSignExtBits(binary));
      }
    } else if (auto* block = boolean->dynCast<Block>()) {
      if (block->type == i32 && block->list.size() > 0) {
        block->list.back() = optimizeBoolean(block->list.back());
      }
    } else if (auto* iff = boolean->dynCast<If>()) {
      if (iff->type == i32) {
        iff->ifTrue = optimizeBoolean(iff->ifTrue);
        iff->ifFalse = optimizeBoolean(iff->ifFalse);
      }
    }
    // TODO: recurse into br values?
    return boolean;
  }

  // find added constants in an expression tree, including multiplied/shifted, and combine them
  // note that we ignore division/shift-right, as rounding makes this nonlinear, so not a valid opt
  Expression* optimizeAddedConstants(Binary* binary) {
    int32_t constant = 0;
    std::vector<Const*> constants;
    std::function<void (Expression*, int)> seek = [&](Expression* curr, int mul) {
      if (auto* c = curr->dynCast<Const>()) {
        auto value = c->value.geti32();
        if (value != 0) {
          constant += value * mul;
          constants.push_back(c);
        }
      } else if (auto* binary = curr->dynCast<Binary>()) {
        if (binary->op == AddInt32) {
          seek(binary->left, mul);
          seek(binary->right, mul);
          return;
        } else if (binary->op == SubInt32) {
          // if the left is a zero, ignore it, it's how we negate ints
          auto* left = binary->left->dynCast<Const>();
          if (!left || left->value.geti32() != 0) {
            seek(binary->left, mul);
          }
          seek(binary->right, -mul);
          return;
        } else if (binary->op == ShlInt32) {
          if (auto* c = binary->right->dynCast<Const>()) {
            seek(binary->left, mul * Pow2(c->value.geti32()));
            return;
          }
        } else if (binary->op == MulInt32) {
          if (auto* c = binary->left->dynCast<Const>()) {
            seek(binary->right, mul * c->value.geti32());
            return;
          } else if (auto* c = binary->right->dynCast<Const>()) {
            seek(binary->left, mul * c->value.geti32());
            return;
          }
        }
      }
    };
    // find all factors
    seek(binary, 1);
    if (constants.size() <= 1) {
      // nothing much to do, except for the trivial case of adding/subbing a zero
      if (auto* c = binary->right->dynCast<Const>()) {
        if (c->value.geti32() == 0) {
          return binary->left;
        }
      }
      return nullptr;
    }
    // wipe out all constants, we'll replace with a single added one
    for (auto* c : constants) {
      c->value = Literal(int32_t(0));
    }
    // remove added/subbed zeros
    struct ZeroRemover : public PostWalker<ZeroRemover, Visitor<ZeroRemover>> {
      // TODO: we could save the binarys and costs we drop, and reuse them later

      PassOptions& passOptions;

      ZeroRemover(PassOptions& passOptions) : passOptions(passOptions) {}

      void visitBinary(Binary* curr) {
        auto* left = curr->left->dynCast<Const>();
        auto* right = curr->right->dynCast<Const>();
        if (curr->op == AddInt32) {
          if (left && left->value.geti32() == 0) {
            replaceCurrent(curr->right);
            return;
          }
          if (right && right->value.geti32() == 0) {
            replaceCurrent(curr->left);
            return;
          }
        } else if (curr->op == SubInt32) {
          // we must leave a left zero, as it is how we negate ints
          if (right && right->value.geti32() == 0) {
            replaceCurrent(curr->left);
            return;
          }
        } else if (curr->op == ShlInt32) {
          // shifting a 0 is a 0, unless the shift has side effects
          if (left && left->value.geti32() == 0 && !EffectAnalyzer(passOptions, curr->right).hasSideEffects()) {
            replaceCurrent(left);
            return;
          }
        } else if (curr->op == MulInt32) {
          // multiplying by zero is a zero, unless the other side has side effects
          if (left && left->value.geti32() == 0 && !EffectAnalyzer(passOptions, curr->right).hasSideEffects()) {
            replaceCurrent(left);
            return;
          }
          if (right && right->value.geti32() == 0 && !EffectAnalyzer(passOptions, curr->left).hasSideEffects()) {
            replaceCurrent(right);
            return;
          }
        }
      }
    };
    Expression* walked = binary;
    ZeroRemover(getPassOptions()).walk(walked);
    if (constant == 0) return walked; // nothing more to do
    if (auto* c = walked->dynCast<Const>()) {
      assert(c->value.geti32() == 0);
      c->value = Literal(constant);
      return c;
    }
    Builder builder(*getModule());
    return builder.makeBinary(AddInt32,
      walked,
      builder.makeConst(Literal(constant))
    );
  }

  //   expensive1 | expensive2 can be turned into expensive1 ? 1 : expensive2, and
  //   expensive | cheap     can be turned into cheap     ? 1 : expensive,
  // so that we can avoid one expensive computation, if it has no side effects.
  Expression* conditionalizeExpensiveOnBitwise(Binary* binary) {
    // this operation can increase code size, so don't always do it
    auto& options = getPassRunner()->options;
    if (options.optimizeLevel < 2 || options.shrinkLevel > 0) return nullptr;
    const auto MIN_COST = 7;
    assert(binary->op == AndInt32 || binary->op == OrInt32);
    if (binary->right->is<Const>()) return nullptr; // trivial
    // bitwise logical operator on two non-numerical values, check if they are boolean
    auto* left = binary->left;
    auto* right = binary->right;
    if (!Properties::emitsBoolean(left) || !Properties::emitsBoolean(right)) return nullptr;
    auto leftEffects = EffectAnalyzer(getPassOptions(), left).hasSideEffects();
    auto rightEffects = EffectAnalyzer(getPassOptions(), right).hasSideEffects();
    if (leftEffects && rightEffects) return nullptr; // both must execute
   // canonicalize with side effects, if any, happening on the left
    if (rightEffects) {
      if (CostAnalyzer(left).cost < MIN_COST) return nullptr; // avoidable code is too cheap
      std::swap(left, right);
    } else if (leftEffects) {
      if (CostAnalyzer(right).cost < MIN_COST) return nullptr; // avoidable code is too cheap
    } else {
      // no side effects, reorder based on cost estimation
      auto leftCost = CostAnalyzer(left).cost;
      auto rightCost = CostAnalyzer(right).cost;
      if (std::max(leftCost, rightCost) < MIN_COST) return nullptr; // avoidable code is too cheap
      // canonicalize with expensive code on the right
      if (leftCost > rightCost) {
        std::swap(left, right);
      }
    }
    // worth it! perform conditionalization
    Builder builder(*getModule());
    if (binary->op == OrInt32) {
      return builder.makeIf(left, builder.makeConst(Literal(int32_t(1))), right);
    } else { // &
      return builder.makeIf(left, right, builder.makeConst(Literal(int32_t(0))));
    }
  }

  // fold constant factors into the offset
  void optimizeMemoryAccess(Expression*& ptr, Address& offset) {
    // ptr may be a const, but it isn't worth folding that in (we still have a const); in fact,
    // it's better to do the opposite for gzip purposes as well as for readability.
    auto* last = ptr->dynCast<Const>();
    if (last) {
      last->value = Literal(int32_t(last->value.geti32() + offset));
      offset = 0;
    }
  }

  Expression* makeZeroExt(Expression* curr, int32_t bits) {
    Builder builder(*getModule());
    return builder.makeBinary(AndInt32, curr, builder.makeConst(Literal(lowBitMask(bits))));
  }

  // given an "almost" sign extend - either a proper one, or it
  // has too many shifts left - we remove the sig extend. If there are
  // too many shifts, we split the shifts first, so this removes the
  // two sign extend shifts and adds one (smaller one)
  Expression* removeAlmostSignExt(Binary* outer) {
    auto* inner = outer->left->cast<Binary>();
    auto* outerConst = outer->right->cast<Const>();
    auto* innerConst = inner->right->cast<Const>();
    auto* value = inner->left;
    if (outerConst->value == innerConst->value) return value;
    // add a shift, by reusing the existing node
    innerConst->value = innerConst->value.sub(outerConst->value);
    return inner;
  }
};

Pass *createOptimizeInstructionsPass() {
  return new OptimizeInstructions();
}

} // namespace wasm
