/*
 * Copyright (c) 2021, Jan de Visser <jan@de-visser.net>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRegex/Regex.h>
#include <LibSQL/AST/AST.h>
#include <LibSQL/Database.h>

namespace SQL::AST {

static const String s_posix_basic_metacharacters = ".^$*[]+\\";

Value Expression::evaluate(ExecutionContext&) const
{
    return Value::null();
}

Value NumericLiteral::evaluate(ExecutionContext& context) const
{
    if (context.result->has_error())
        return Value::null();
    Value ret(SQLType::Float);
    ret = value();
    return ret;
}

Value StringLiteral::evaluate(ExecutionContext& context) const
{
    if (context.result->has_error())
        return Value::null();
    Value ret(SQLType::Text);
    ret = value();
    return ret;
}

Value NullLiteral::evaluate(ExecutionContext&) const
{
    return Value::null();
}

Value NestedExpression::evaluate(ExecutionContext& context) const
{
    if (context.result->has_error())
        return Value::null();
    return expression()->evaluate(context);
}

Value ChainedExpression::evaluate(ExecutionContext& context) const
{
    if (context.result->has_error())
        return Value::null();
    Value ret(SQLType::Tuple);
    Vector<Value> values;
    for (auto& expression : expressions()) {
        values.append(expression.evaluate(context));
    }
    ret = values;
    return ret;
}

Value BinaryOperatorExpression::evaluate(ExecutionContext& context) const
{
    if (context.result->has_error())
        return Value::null();
    Value lhs_value = lhs()->evaluate(context);
    Value rhs_value = rhs()->evaluate(context);
    switch (type()) {
    case BinaryOperator::Concatenate: {
        if (lhs_value.type() != SQLType::Text) {
            context.result->set_error(SQLErrorCode::BooleanOperatorTypeMismatch, BinaryOperator_name(type()));
            return Value::null();
        }
        AK::StringBuilder builder;
        builder.append(lhs_value.to_string());
        builder.append(rhs_value.to_string());
        return Value(builder.to_string());
    }
    case BinaryOperator::Multiplication:
        return lhs_value.multiply(rhs_value);
    case BinaryOperator::Division:
        return lhs_value.divide(rhs_value);
    case BinaryOperator::Modulo:
        return lhs_value.modulo(rhs_value);
    case BinaryOperator::Plus:
        return lhs_value.add(rhs_value);
    case BinaryOperator::Minus:
        return lhs_value.subtract(rhs_value);
    case BinaryOperator::ShiftLeft:
        return lhs_value.shift_left(rhs_value);
    case BinaryOperator::ShiftRight:
        return lhs_value.shift_right(rhs_value);
    case BinaryOperator::BitwiseAnd:
        return lhs_value.bitwise_and(rhs_value);
    case BinaryOperator::BitwiseOr:
        return lhs_value.bitwise_or(rhs_value);
    case BinaryOperator::LessThan:
        return Value(lhs_value.compare(rhs_value) < 0);
    case BinaryOperator::LessThanEquals:
        return Value(lhs_value.compare(rhs_value) <= 0);
    case BinaryOperator::GreaterThan:
        return Value(lhs_value.compare(rhs_value) > 0);
    case BinaryOperator::GreaterThanEquals:
        return Value(lhs_value.compare(rhs_value) >= 0);
    case BinaryOperator::Equals:
        return Value(lhs_value.compare(rhs_value) == 0);
    case BinaryOperator::NotEquals:
        return Value(lhs_value.compare(rhs_value) != 0);
    case BinaryOperator::And: {
        auto lhs_bool_maybe = lhs_value.to_bool();
        auto rhs_bool_maybe = rhs_value.to_bool();
        if (!lhs_bool_maybe.has_value() || !rhs_bool_maybe.has_value()) {
            context.result->set_error(SQLErrorCode::BooleanOperatorTypeMismatch, BinaryOperator_name(type()));
            return Value::null();
        }
        return Value(lhs_bool_maybe.release_value() && rhs_bool_maybe.release_value());
    }
    case BinaryOperator::Or: {
        auto lhs_bool_maybe = lhs_value.to_bool();
        auto rhs_bool_maybe = rhs_value.to_bool();
        if (!lhs_bool_maybe.has_value() || !rhs_bool_maybe.has_value()) {
            context.result->set_error(SQLErrorCode::BooleanOperatorTypeMismatch, BinaryOperator_name(type()));
            return Value::null();
        }
        return Value(lhs_bool_maybe.release_value() || rhs_bool_maybe.release_value());
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

Value UnaryOperatorExpression::evaluate(ExecutionContext& context) const
{
    if (context.result->has_error())
        return Value::null();
    Value expression_value = NestedExpression::evaluate(context);
    switch (type()) {
    case UnaryOperator::Plus:
        if (expression_value.type() == SQLType::Integer || expression_value.type() == SQLType::Float)
            return expression_value;
        context.result->set_error(SQLErrorCode::NumericOperatorTypeMismatch, UnaryOperator_name(type()));
        return Value::null();
    case UnaryOperator::Minus:
        if (expression_value.type() == SQLType::Integer) {
            expression_value = -int(expression_value);
            return expression_value;
        }
        if (expression_value.type() == SQLType::Float) {
            expression_value = -double(expression_value);
            return expression_value;
        }
        context.result->set_error(SQLErrorCode::NumericOperatorTypeMismatch, UnaryOperator_name(type()));
        return Value::null();
    case UnaryOperator::Not:
        if (expression_value.type() == SQLType::Boolean) {
            expression_value = !bool(expression_value);
            return expression_value;
        }
        context.result->set_error(SQLErrorCode::BooleanOperatorTypeMismatch, UnaryOperator_name(type()));
        return Value::null();
    case UnaryOperator::BitwiseNot:
        if (expression_value.type() == SQLType::Integer) {
            expression_value = ~u32(expression_value);
            return expression_value;
        }
        context.result->set_error(SQLErrorCode::IntegerOperatorTypeMismatch, UnaryOperator_name(type()));
        return Value::null();
    }
    VERIFY_NOT_REACHED();
}

Value ColumnNameExpression::evaluate(ExecutionContext& context) const
{
    if (!context.current_row) {
        context.result->set_error(SQLErrorCode::SyntaxError, column_name());
        return Value::null();
    }
    auto& descriptor = *context.current_row->descriptor();
    VERIFY(context.current_row->size() == descriptor.size());
    Optional<size_t> index_in_row;
    for (auto ix = 0u; ix < context.current_row->size(); ix++) {
        auto& column_descriptor = descriptor[ix];
        if (!table_name().is_empty() && column_descriptor.table != table_name())
            continue;
        if (column_descriptor.name == column_name()) {
            if (index_in_row.has_value()) {
                context.result->set_error(SQLErrorCode::AmbiguousColumnName, column_name());
                return Value::null();
            }
            index_in_row = ix;
        }
    }
    if (index_in_row.has_value())
        return (*context.current_row)[index_in_row.value()];
    context.result->set_error(SQLErrorCode::ColumnDoesNotExist, column_name());
    return Value::null();
}

Value MatchExpression::evaluate(ExecutionContext& context) const
{
    if (context.result->has_error())
        return Value::null();
    switch (type()) {
    case MatchOperator::Like: {
        Value lhs_value = lhs()->evaluate(context);
        Value rhs_value = rhs()->evaluate(context);
        char escape_char = '\0';
        if (escape()) {
            auto escape_str = escape()->evaluate(context).to_string();
            if (escape_str.length() != 1) {
                context.result->set_error(SQLErrorCode::SyntaxError, "ESCAPE should be a single character");
                return Value::null();
            }
            escape_char = escape_str[0];
        }

        // Compile the pattern into a simple regex.
        // https://sqlite.org/lang_expr.html#the_like_glob_regexp_and_match_operators
        bool escaped = false;
        AK::StringBuilder builder;
        builder.append('^');
        for (auto c : rhs_value.to_string()) {
            if (escape() && c == escape_char && !escaped) {
                escaped = true;
            } else if (s_posix_basic_metacharacters.contains(c)) {
                escaped = false;
                builder.append('\\');
                builder.append(c);
            } else if (c == '_' && !escaped) {
                builder.append('.');
            } else if (c == '%' && !escaped) {
                builder.append(".*");
            } else {
                escaped = false;
                builder.append(c);
            }
        }
        builder.append('$');
        // FIXME: We should probably cache this regex.
        auto regex = Regex<PosixBasic>(builder.build());
        auto result = regex.match(lhs_value.to_string(), PosixFlags::Insensitive | PosixFlags::Unicode);
        return Value(invert_expression() ? !result.success : result.success);
    }
    case MatchOperator::Regexp: {
        Value lhs_value = lhs()->evaluate(context);
        Value rhs_value = rhs()->evaluate(context);

        auto regex = Regex<PosixExtended>(rhs_value.to_string());
        auto err = regex.parser_result.error;
        if (err != regex::Error::NoError) {
            StringBuilder builder;
            builder.append("Regular expression: ");
            builder.append(get_error_string(err));

            context.result->set_error(SQLErrorCode::SyntaxError, builder.build());
            return Value(false);
        }

        auto result = regex.match(lhs_value.to_string(), PosixFlags::Insensitive | PosixFlags::Unicode);
        return Value(invert_expression() ? !result.success : result.success);
    }
    case MatchOperator::Glob:
    case MatchOperator::Match:
    default:
        VERIFY_NOT_REACHED();
    }
    return Value::null();
}

}
