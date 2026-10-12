// The binary and unary operators (13.5–13.15): ApplyStringOrNumericBinaryOperator and the Number::/BigInt::
// operations of 6.1.6.
#include <cmath>
#include <cstdint>

#include "interp/interpreter.h"
#include "runtime/bigint.h"
#include "runtime/errors.h"
#include "runtime/operations.h"
#include "runtime/string.h"

namespace snowman::interp {

using ast::BinaryOperator;

namespace {

// Number::exponentiate (6.1.6.1.3): pow() except where the spec says NaN.
double numberExponentiate(double base, double exponent) {
  if (std::isnan(exponent)) return NAN;
  if (exponent == 0) return 1;
  if (std::isinf(exponent) && std::fabs(base) == 1) return NAN;
  return std::pow(base, exponent);
}

// Shift counts and exponents beyond this would build a BigInt no test can use: a RangeError instead.
constexpr uint64_t kMaxBigIntBits = uint64_t(1) << 30;

Result<Value> bigintOperation(BinaryOperator op, BigInt* x, BigInt* y) {
  switch (op) {
    case BinaryOperator::Exponent: {
      if (y->isNegative()) return ThrowRangeError("Exponent must be non-negative");
      bool trivialBase = x->isZero() || (x->magnitude().size() == 1 && x->magnitude()[0] == 1);
      if (!trivialBase && (y->magnitude().size() > 1 || (!y->isZero() && y->magnitude()[0] > kMaxBigIntBits))) {
        return ThrowRangeError("Maximum BigInt size exceeded");
      }
      return Value(BigInt::exponentiate(x, y));
    }
    case BinaryOperator::Multiply:
      return Value(BigInt::multiply(x, y));
    case BinaryOperator::Divide:
      if (y->isZero()) return ThrowRangeError("Division by zero");
      return Value(BigInt::divide(x, y));
    case BinaryOperator::Remainder:
      if (y->isZero()) return ThrowRangeError("Division by zero");
      return Value(BigInt::remainder(x, y));
    case BinaryOperator::Add:
      return Value(BigInt::add(x, y));
    case BinaryOperator::Subtract:
      return Value(BigInt::subtract(x, y));
    case BinaryOperator::ShiftLeft:
    case BinaryOperator::ShiftRight: {
      bool grows = (op == BinaryOperator::ShiftLeft) != y->isNegative();
      if (grows && !x->isZero() && (y->magnitude().size() > 1 || y->magnitude()[0] > kMaxBigIntBits)) {
        return ThrowRangeError("Maximum BigInt size exceeded");
      }
      if (!grows && (y->magnitude().size() > 1 || y->magnitude()[0] > kMaxBigIntBits)) {
        return Value(BigInt::fromInt64(x->isNegative() ? -1 : 0));
      }
      return Value(op == BinaryOperator::ShiftLeft ? BigInt::leftShift(x, y) : BigInt::signedRightShift(x, y));
    }
    case BinaryOperator::ShiftRightUnsigned:
      return ThrowTypeError("BigInts have no unsigned right shift, use >> instead");
    case BinaryOperator::BitAnd:
      return Value(BigInt::bitwiseAnd(x, y));
    case BinaryOperator::BitXor:
      return Value(BigInt::bitwiseXor(x, y));
    case BinaryOperator::BitOr:
      return Value(BigInt::bitwiseOr(x, y));
    default:
      return Value::undefined();
  }
}

Value numberOperation(BinaryOperator op, double x, double y) {
  switch (op) {
    case BinaryOperator::Exponent:
      return Value::number(numberExponentiate(x, y));
    case BinaryOperator::Multiply:
      return Value::number(x * y);
    case BinaryOperator::Divide:
      return Value::number(x / y);
    case BinaryOperator::Remainder:
      return Value::number(std::fmod(x, y));
    case BinaryOperator::Add:
      return Value::number(x + y);
    case BinaryOperator::Subtract:
      return Value::number(x - y);
    case BinaryOperator::ShiftLeft:
      return Value::number(double(int32_t(uint32_t(DoubleToInt32(x)) << (DoubleToUint32(y) & 31))));
    case BinaryOperator::ShiftRight:
      return Value::number(double(DoubleToInt32(x) >> (DoubleToUint32(y) & 31)));
    case BinaryOperator::ShiftRightUnsigned:
      return Value::number(double(DoubleToUint32(x) >> (DoubleToUint32(y) & 31)));
    case BinaryOperator::BitAnd:
      return Value::number(double(DoubleToInt32(x) & DoubleToInt32(y)));
    case BinaryOperator::BitXor:
      return Value::number(double(DoubleToInt32(x) ^ DoubleToInt32(y)));
    case BinaryOperator::BitOr:
      return Value::number(double(DoubleToInt32(x) | DoubleToInt32(y)));
    default:
      return Value::undefined();
  }
}

}  // namespace

// 13.15.3 ApplyStringOrNumericBinaryOperator
Result<Value> ApplyStringOrNumericBinaryOperator(Value lval, BinaryOperator op, Value rval) {
  if (op == BinaryOperator::Add) {
    if (lval.isNumber() && rval.isNumber()) return Value::number(lval.asNumber() + rval.asNumber());
    if (lval.isString() && rval.isString()) return Value(concat(lval.asString(), rval.asString()));
    Value lprim = TRY(ToPrimitive(lval));
    Value rprim = TRY(ToPrimitive(rval));
    if (lprim.isString() || rprim.isString()) {
      String* lstr = TRY(ToString(lprim));
      String* rstr = TRY(ToString(rprim));
      return Value(concat(lstr, rstr));
    }
    lval = lprim;
    rval = rprim;
  }
  if (lval.isNumber() && rval.isNumber()) return numberOperation(op, lval.asNumber(), rval.asNumber());
  Value lnum = TRY(ToNumeric(lval));
  Value rnum = TRY(ToNumeric(rval));
  if (lnum.type() != rnum.type()) return ThrowTypeError("Cannot mix BigInt and other types, use explicit conversions");
  if (lnum.isBigInt()) return bigintOperation(op, lnum.asBigInt(), rnum.asBigInt());
  return numberOperation(op, lnum.asNumber(), rnum.asNumber());
}

Result<Value> evaluateBinaryOperator(BinaryOperator op, Value lval, Value rval) {
  switch (op) {
    case BinaryOperator::Equal:
      return Value::boolean(TRY(IsLooselyEqual(lval, rval)));
    case BinaryOperator::NotEqual:
      return Value::boolean(!TRY(IsLooselyEqual(lval, rval)));
    case BinaryOperator::StrictEqual:
      return Value::boolean(IsStrictlyEqual(lval, rval));
    case BinaryOperator::StrictNotEqual:
      return Value::boolean(!IsStrictlyEqual(lval, rval));
    case BinaryOperator::Less: {
      if (lval.isNumber() && rval.isNumber()) return Value::boolean(lval.asNumber() < rval.asNumber());
      auto r = TRY(IsLessThan(lval, rval, true));
      return Value::boolean(r.value_or(false));
    }
    case BinaryOperator::Greater: {
      if (lval.isNumber() && rval.isNumber()) return Value::boolean(lval.asNumber() > rval.asNumber());
      auto r = TRY(IsLessThan(rval, lval, false));
      return Value::boolean(r.value_or(false));
    }
    case BinaryOperator::LessEqual: {
      if (lval.isNumber() && rval.isNumber()) return Value::boolean(lval.asNumber() <= rval.asNumber());
      auto r = TRY(IsLessThan(rval, lval, false));
      return Value::boolean(r.has_value() && !*r);
    }
    case BinaryOperator::GreaterEqual: {
      if (lval.isNumber() && rval.isNumber()) return Value::boolean(lval.asNumber() >= rval.asNumber());
      auto r = TRY(IsLessThan(lval, rval, true));
      return Value::boolean(r.has_value() && !*r);
    }
    case BinaryOperator::Instanceof:
      return Value::boolean(TRY(InstanceofOperator(lval, rval)));
    case BinaryOperator::In: {
      if (!rval.isObject()) return ThrowTypeError("Cannot use 'in' operator to search for a key in a non-object");
      PropertyKey key = TRY(ToPropertyKey(lval));
      return Value::boolean(TRY(HasProperty(rval.asObject(), key)));
    }
    default:
      return ApplyStringOrNumericBinaryOperator(lval, op, rval);
  }
}

Result<Value> evaluateUnaryNumeric(ast::UnaryOperator op, Value value) {
  switch (op) {
    case ast::UnaryOperator::Plus:
      return Value::number(TRY(ToNumber(value)));
    case ast::UnaryOperator::Minus: {
      Value numeric = TRY(ToNumeric(value));
      if (numeric.isBigInt()) return Value(BigInt::unaryMinus(numeric.asBigInt()));
      return Value::number(-numeric.asNumber());
    }
    case ast::UnaryOperator::BitNot: {
      Value numeric = TRY(ToNumeric(value));
      if (numeric.isBigInt()) return Value(BigInt::bitwiseNot(numeric.asBigInt()));
      return Value::number(double(~DoubleToInt32(numeric.asNumber())));
    }
    default:
      return Value::undefined();
  }
}

BinaryOperator binaryOperatorFor(ast::AssignmentOperator op) {
  switch (op) {
    case ast::AssignmentOperator::Add:
      return BinaryOperator::Add;
    case ast::AssignmentOperator::Subtract:
      return BinaryOperator::Subtract;
    case ast::AssignmentOperator::Multiply:
      return BinaryOperator::Multiply;
    case ast::AssignmentOperator::Divide:
      return BinaryOperator::Divide;
    case ast::AssignmentOperator::Remainder:
      return BinaryOperator::Remainder;
    case ast::AssignmentOperator::Exponent:
      return BinaryOperator::Exponent;
    case ast::AssignmentOperator::ShiftLeft:
      return BinaryOperator::ShiftLeft;
    case ast::AssignmentOperator::ShiftRight:
      return BinaryOperator::ShiftRight;
    case ast::AssignmentOperator::ShiftRightUnsigned:
      return BinaryOperator::ShiftRightUnsigned;
    case ast::AssignmentOperator::BitOr:
      return BinaryOperator::BitOr;
    case ast::AssignmentOperator::BitXor:
      return BinaryOperator::BitXor;
    case ast::AssignmentOperator::BitAnd:
      return BinaryOperator::BitAnd;
    default:
      return BinaryOperator::Add;
  }
}

}  // namespace snowman::interp
