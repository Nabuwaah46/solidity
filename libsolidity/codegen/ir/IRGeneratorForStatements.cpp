/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
 * Component that translates Solidity code into Yul at statement level and below.
 */

#include <libsolidity/codegen/ir/IRGeneratorForStatements.h>

#include <libsolidity/codegen/ABIFunctions.h>
#include <libsolidity/codegen/ir/IRGenerationContext.h>
#include <libsolidity/codegen/ir/IRLValue.h>
#include <libsolidity/codegen/ir/IRVariable.h>
#include <libsolidity/codegen/YulUtilFunctions.h>
#include <libsolidity/codegen/ABIFunctions.h>
#include <libsolidity/codegen/CompilerUtils.h>
#include <libsolidity/codegen/ReturnInfo.h>
#include <libsolidity/ast/TypeProvider.h>

#include <libevmasm/GasMeter.h>

#include <libyul/AsmPrinter.h>
#include <libyul/AsmData.h>
#include <libyul/Dialect.h>
#include <libyul/optimiser/ASTCopier.h>

#include <libsolutil/Whiskers.h>
#include <libsolutil/StringUtils.h>
#include <libsolutil/Keccak256.h>
#include <libsolutil/Visitor.h>

#include <boost/range/adaptor/reversed.hpp>
#include <boost/range/adaptor/transformed.hpp>

using namespace std;
using namespace solidity;
using namespace solidity::util;
using namespace solidity::frontend;
using namespace std::string_literals;

namespace
{

struct CopyTranslate: public yul::ASTCopier
{
	using ExternalRefsMap = std::map<yul::Identifier const*, InlineAssemblyAnnotation::ExternalIdentifierInfo>;

	CopyTranslate(yul::Dialect const& _dialect, IRGenerationContext& _context, ExternalRefsMap const& _references):
		m_dialect(_dialect), m_context(_context), m_references(_references) {}

	using ASTCopier::operator();

	yul::Expression operator()(yul::Identifier const& _identifier) override
	{
		if (m_references.count(&_identifier))
		{
			auto const& reference = m_references.at(&_identifier);
			auto const varDecl = dynamic_cast<VariableDeclaration const*>(reference.declaration);
			solUnimplementedAssert(varDecl, "");

			if (reference.isOffset || reference.isSlot)
			{
				solAssert(reference.isOffset != reference.isSlot, "");

				pair<u256, unsigned> slot_offset = m_context.storageLocationOfVariable(*varDecl);

				string const value = reference.isSlot ?
					slot_offset.first.str() :
					to_string(slot_offset.second);

				return yul::Literal{
					_identifier.location,
					yul::LiteralKind::Number,
					yul::YulString{value},
					{}
				};
			}
		}
		return ASTCopier::operator()(_identifier);
	}

	yul::YulString translateIdentifier(yul::YulString _name) override
	{
		// Strictly, the dialect used by inline assembly (m_dialect) could be different
		// from the Yul dialect we are compiling to. So we are assuming here that the builtin
		// functions are identical. This should not be a problem for now since everything
		// is EVM anyway.
		if (m_dialect.builtin(_name))
			return _name;
		else
			return yul::YulString{"usr$" + _name.str()};
	}

	yul::Identifier translate(yul::Identifier const& _identifier) override
	{
		if (!m_references.count(&_identifier))
			return ASTCopier::translate(_identifier);

		auto const& reference = m_references.at(&_identifier);
		auto const varDecl = dynamic_cast<VariableDeclaration const*>(reference.declaration);
		solUnimplementedAssert(varDecl, "");

		solAssert(
			reference.isOffset == false && reference.isSlot == false,
			"Should not be called for offset/slot"
		);

		return yul::Identifier{
			_identifier.location,
			yul::YulString{m_context.localVariable(*varDecl).name()}
		};
	}

private:
	yul::Dialect const& m_dialect;
	IRGenerationContext& m_context;
	ExternalRefsMap const& m_references;
};

}

string IRGeneratorForStatements::code() const
{
	solAssert(!m_currentLValue, "LValue not reset!");
	return m_code.str();
}

void IRGeneratorForStatements::initializeStateVar(VariableDeclaration const& _varDecl)
{
	solAssert(m_context.isStateVariable(_varDecl), "Must be a state variable.");
	solAssert(!_varDecl.isConstant(), "");
	solAssert(!_varDecl.immutable(), "");
	if (_varDecl.value())
	{
		_varDecl.value()->accept(*this);
		writeToLValue(IRLValue{
			*_varDecl.annotation().type,
			IRLValue::Storage{
				util::toCompactHexWithPrefix(m_context.storageLocationOfVariable(_varDecl).first),
				m_context.storageLocationOfVariable(_varDecl).second
			}
		}, *_varDecl.value());
	}
}

void IRGeneratorForStatements::initializeLocalVar(VariableDeclaration const& _varDecl)
{
	solAssert(m_context.isLocalVariable(_varDecl), "Must be a local variable.");

	auto const* type = _varDecl.type();
	if (auto const* refType = dynamic_cast<ReferenceType const*>(type))
		if (refType->dataStoredIn(DataLocation::Storage) && refType->isPointer())
			return;

	IRVariable zero = zeroValue(*type);
	assign(m_context.localVariable(_varDecl), zero);
}

IRVariable IRGeneratorForStatements::evaluateExpression(Expression const& _expression, Type const& _targetType)
{
	_expression.accept(*this);
	IRVariable variable{m_context.newYulVariable(), _targetType};
	define(variable, _expression);
	return variable;
}

void IRGeneratorForStatements::endVisit(VariableDeclarationStatement const& _varDeclStatement)
{
	if (Expression const* expression = _varDeclStatement.initialValue())
	{
		if (_varDeclStatement.declarations().size() > 1)
		{
			auto const* tupleType = dynamic_cast<TupleType const*>(expression->annotation().type);
			solAssert(tupleType, "Expected expression of tuple type.");
			solAssert(_varDeclStatement.declarations().size() == tupleType->components().size(), "Invalid number of tuple components.");
			for (size_t i = 0; i < _varDeclStatement.declarations().size(); ++i)
				if (auto const& decl = _varDeclStatement.declarations()[i])
				{
					solAssert(tupleType->components()[i], "");
					define(m_context.addLocalVariable(*decl), IRVariable(*expression).tupleComponent(i));
				}
		}
		else
		{
			VariableDeclaration const& varDecl = *_varDeclStatement.declarations().front();
			define(m_context.addLocalVariable(varDecl), *expression);
		}
	}
	else
		for (auto const& decl: _varDeclStatement.declarations())
			if (decl)
			{
				declare(m_context.addLocalVariable(*decl));
				initializeLocalVar(*decl);
			}
}

bool IRGeneratorForStatements::visit(Conditional const& _conditional)
{
	_conditional.condition().accept(*this);

	string condition = expressionAsType(_conditional.condition(), *TypeProvider::boolean());
	declare(_conditional);

	m_code << "switch " << condition << "\n" "case 0 {\n";
	_conditional.falseExpression().accept(*this);
	assign(_conditional, _conditional.falseExpression());
	m_code << "}\n" "default {\n";
	_conditional.trueExpression().accept(*this);
	assign(_conditional, _conditional.trueExpression());
	m_code << "}\n";

	return false;
}

bool IRGeneratorForStatements::visit(Assignment const& _assignment)
{
	_assignment.rightHandSide().accept(*this);
	Type const* intermediateType = type(_assignment.rightHandSide()).closestTemporaryType(
		&type(_assignment.leftHandSide())
	);
	IRVariable value = convert(_assignment.rightHandSide(), *intermediateType);

	_assignment.leftHandSide().accept(*this);
	solAssert(!!m_currentLValue, "LValue not retrieved.");

	if (_assignment.assignmentOperator() != Token::Assign)
	{
		solAssert(type(_assignment.leftHandSide()) == *intermediateType, "");
		solAssert(intermediateType->isValueType(), "Compound operators only available for value types.");

		IRVariable leftIntermediate = readFromLValue(*m_currentLValue);
		m_code << value.name() << " := " << binaryOperation(
			TokenTraits::AssignmentToBinaryOp(_assignment.assignmentOperator()),
			*intermediateType,
			leftIntermediate.name(),
			value.name()
		);
	}

	writeToLValue(*m_currentLValue, value);
	m_currentLValue.reset();
	if (*_assignment.annotation().type != *TypeProvider::emptyTuple())
		define(_assignment, value);

	return false;
}

bool IRGeneratorForStatements::visit(TupleExpression const& _tuple)
{
	if (_tuple.isInlineArray())
		solUnimplementedAssert(false, "");
	else
	{
		bool willBeWrittenTo = _tuple.annotation().willBeWrittenTo;
		if (willBeWrittenTo)
			solAssert(!m_currentLValue, "");
		if (_tuple.components().size() == 1)
		{
			solAssert(_tuple.components().front(), "");
			_tuple.components().front()->accept(*this);
			if (willBeWrittenTo)
				solAssert(!!m_currentLValue, "");
			else
				define(_tuple, *_tuple.components().front());
		}
		else
		{
			vector<optional<IRLValue>> lvalues;
			for (size_t i = 0; i < _tuple.components().size(); ++i)
				if (auto const& component = _tuple.components()[i])
				{
					component->accept(*this);
					if (willBeWrittenTo)
					{
						solAssert(!!m_currentLValue, "");
						lvalues.emplace_back(std::move(m_currentLValue));
						m_currentLValue.reset();
					}
					else
						define(IRVariable(_tuple).tupleComponent(i), *component);
				}
				else if (willBeWrittenTo)
					lvalues.emplace_back();

			if (_tuple.annotation().willBeWrittenTo)
				m_currentLValue.emplace(IRLValue{
					*_tuple.annotation().type,
					IRLValue::Tuple{std::move(lvalues)}
				});
		}
	}
	return false;
}

bool IRGeneratorForStatements::visit(IfStatement const& _ifStatement)
{
	_ifStatement.condition().accept(*this);
	string condition = expressionAsType(_ifStatement.condition(), *TypeProvider::boolean());

	if (_ifStatement.falseStatement())
	{
		m_code << "switch " << condition << "\n" "case 0 {\n";
		_ifStatement.falseStatement()->accept(*this);
		m_code << "}\n" "default {\n";
	}
	else
		m_code << "if " << condition << " {\n";
	_ifStatement.trueStatement().accept(*this);
	m_code << "}\n";

	return false;
}

bool IRGeneratorForStatements::visit(ForStatement const& _forStatement)
{
	generateLoop(
		_forStatement.body(),
		_forStatement.condition(),
		_forStatement.initializationExpression(),
		_forStatement.loopExpression()
	);

	return false;
}

bool IRGeneratorForStatements::visit(WhileStatement const& _whileStatement)
{
	generateLoop(
		_whileStatement.body(),
		&_whileStatement.condition(),
		nullptr,
		nullptr,
		_whileStatement.isDoWhile()
	);

	return false;
}

bool IRGeneratorForStatements::visit(Continue const&)
{
	m_code << "continue\n";
	return false;
}

bool IRGeneratorForStatements::visit(Break const&)
{
	m_code << "break\n";
	return false;
}

void IRGeneratorForStatements::endVisit(Return const& _return)
{
	if (Expression const* value = _return.expression())
	{
		solAssert(_return.annotation().functionReturnParameters, "Invalid return parameters pointer.");
		vector<ASTPointer<VariableDeclaration>> const& returnParameters =
			_return.annotation().functionReturnParameters->parameters();
		if (returnParameters.size() > 1)
			for (size_t i = 0; i < returnParameters.size(); ++i)
				assign(m_context.localVariable(*returnParameters[i]), IRVariable(*value).tupleComponent(i));
		else if (returnParameters.size() == 1)
			assign(m_context.localVariable(*returnParameters.front()), *value);
	}
	m_code << "leave\n";
}

void IRGeneratorForStatements::endVisit(UnaryOperation const& _unaryOperation)
{
	Type const& resultType = type(_unaryOperation);
	Token const op = _unaryOperation.getOperator();

	if (op == Token::Delete)
	{
		solAssert(!!m_currentLValue, "LValue not retrieved.");
		std::visit(
			util::GenericVisitor{
				[&](IRLValue::Storage const& _storage) {
					m_code <<
						m_utils.storageSetToZeroFunction(m_currentLValue->type) <<
						"(" <<
						_storage.slot <<
						", " <<
						_storage.offsetString() <<
						")\n";
					m_currentLValue.reset();
				},
				[&](auto const&) {
					IRVariable zeroValue(m_context.newYulVariable(), m_currentLValue->type);
					define(zeroValue) << m_utils.zeroValueFunction(m_currentLValue->type) << "()\n";
					writeToLValue(*m_currentLValue, zeroValue);
					m_currentLValue.reset();
				}
			},
			m_currentLValue->kind
		);
	}
	else if (resultType.category() == Type::Category::RationalNumber)
		define(_unaryOperation) << formatNumber(resultType.literalValue(nullptr)) << "\n";
	else if (resultType.category() == Type::Category::Integer)
	{
		solAssert(resultType == type(_unaryOperation.subExpression()), "Result type doesn't match!");

		if (op == Token::Inc || op == Token::Dec)
		{
			solAssert(!!m_currentLValue, "LValue not retrieved.");
			IRVariable modifiedValue(m_context.newYulVariable(), resultType);
			IRVariable originalValue = readFromLValue(*m_currentLValue);

			define(modifiedValue) <<
				(op == Token::Inc ?
					m_utils.incrementCheckedFunction(resultType) :
					m_utils.decrementCheckedFunction(resultType)
				) <<
				"(" <<
				originalValue.name() <<
				")\n";
			writeToLValue(*m_currentLValue, modifiedValue);
			m_currentLValue.reset();

			define(_unaryOperation, _unaryOperation.isPrefixOperation() ? modifiedValue : originalValue);
		}
		else if (op == Token::BitNot)
			appendSimpleUnaryOperation(_unaryOperation, _unaryOperation.subExpression());
		else if (op == Token::Add)
			// According to SyntaxChecker...
			solAssert(false, "Use of unary + is disallowed.");
		else if (op == Token::Sub)
		{
			IntegerType const& intType = *dynamic_cast<IntegerType const*>(&resultType);
			define(_unaryOperation) <<
				m_utils.negateNumberCheckedFunction(intType) <<
				"(" <<
				IRVariable(_unaryOperation.subExpression()).name() <<
				")\n";
		}
		else
			solUnimplementedAssert(false, "Unary operator not yet implemented");
	}
	else if (resultType.category() == Type::Category::Bool)
	{
		solAssert(
			_unaryOperation.getOperator() != Token::BitNot,
			"Bitwise Negation can't be done on bool!"
		);

		appendSimpleUnaryOperation(_unaryOperation, _unaryOperation.subExpression());
	}
	else
		solUnimplementedAssert(false, "Unary operator not yet implemented");
}

bool IRGeneratorForStatements::visit(BinaryOperation const& _binOp)
{
	solAssert(!!_binOp.annotation().commonType, "");
	TypePointer commonType = _binOp.annotation().commonType;
	langutil::Token op = _binOp.getOperator();

	if (op == Token::And || op == Token::Or)
	{
		// This can short-circuit!
		appendAndOrOperatorCode(_binOp);
		return false;
	}

	_binOp.leftExpression().accept(*this);
	_binOp.rightExpression().accept(*this);

	if (commonType->category() == Type::Category::RationalNumber)
		define(_binOp) << toCompactHexWithPrefix(commonType->literalValue(nullptr)) << "\n";
	else if (TokenTraits::isCompareOp(op))
	{
		if (auto type = dynamic_cast<FunctionType const*>(commonType))
		{
			solAssert(op == Token::Equal || op == Token::NotEqual, "Invalid function pointer comparison!");
			solAssert(type->kind() != FunctionType::Kind::External, "External function comparison not allowed!");
		}

		solAssert(commonType->isValueType(), "");
		bool isSigned = false;
		if (auto type = dynamic_cast<IntegerType const*>(commonType))
			isSigned = type->isSigned();

		string args =
			expressionAsType(_binOp.leftExpression(), *commonType) +
			", " +
			expressionAsType(_binOp.rightExpression(), *commonType);

		string expr;
		if (op == Token::Equal)
			expr = "eq(" + move(args) + ")";
		else if (op == Token::NotEqual)
			expr = "iszero(eq(" + move(args) + "))";
		else if (op == Token::GreaterThanOrEqual)
			expr = "iszero(" + string(isSigned ? "slt(" : "lt(") + move(args) + "))";
		else if (op == Token::LessThanOrEqual)
			expr = "iszero(" + string(isSigned ? "sgt(" : "gt(") + move(args) + "))";
		else if (op == Token::GreaterThan)
			expr = (isSigned ? "sgt(" : "gt(") + move(args) + ")";
		else if (op == Token::LessThan)
			expr = (isSigned ? "slt(" : "lt(") + move(args) + ")";
		else
			solAssert(false, "Unknown comparison operator.");
		define(_binOp) << expr << "\n";
	}
	else
	{
		string left = expressionAsType(_binOp.leftExpression(), *commonType);
		string right = expressionAsType(_binOp.rightExpression(), *commonType);
		define(_binOp) << binaryOperation(_binOp.getOperator(), *commonType, left, right) << "\n";
	}
	return false;
}

void IRGeneratorForStatements::endVisit(FunctionCall const& _functionCall)
{
	solUnimplementedAssert(
		_functionCall.annotation().kind == FunctionCallKind::FunctionCall ||
		_functionCall.annotation().kind == FunctionCallKind::TypeConversion,
		"This type of function call is not yet implemented"
	);

	Type const& funcType = type(_functionCall.expression());

	if (_functionCall.annotation().kind == FunctionCallKind::TypeConversion)
	{
		solAssert(funcType.category() == Type::Category::TypeType, "Expected category to be TypeType");
		solAssert(_functionCall.arguments().size() == 1, "Expected one argument for type conversion");
		define(_functionCall, *_functionCall.arguments().front());
		return;
	}

	FunctionTypePointer functionType = dynamic_cast<FunctionType const*>(&funcType);

	TypePointers parameterTypes = functionType->parameterTypes();
	vector<ASTPointer<Expression const>> const& callArguments = _functionCall.arguments();
	vector<ASTPointer<ASTString>> const& callArgumentNames = _functionCall.names();
	if (!functionType->takesArbitraryParameters())
		solAssert(callArguments.size() == parameterTypes.size(), "");

	vector<ASTPointer<Expression const>> arguments;
	if (callArgumentNames.empty())
		// normal arguments
		arguments = callArguments;
	else
		// named arguments
		for (auto const& parameterName: functionType->parameterNames())
		{
			auto const it = std::find_if(callArgumentNames.cbegin(), callArgumentNames.cend(), [&](ASTPointer<ASTString> const& _argName) {
				return *_argName == parameterName;
			});

			solAssert(it != callArgumentNames.cend(), "");
			arguments.push_back(callArguments[std::distance(callArgumentNames.begin(), it)]);
		}

	if (auto memberAccess = dynamic_cast<MemberAccess const*>(&_functionCall.expression()))
		if (auto expressionType = dynamic_cast<TypeType const*>(memberAccess->expression().annotation().type))
			if (auto contractType = dynamic_cast<ContractType const*>(expressionType->actualType()))
				solUnimplementedAssert(
					!contractType->contractDefinition().isLibrary() || functionType->kind() == FunctionType::Kind::Internal,
					"Only internal function calls implemented for libraries"
				);

	solUnimplementedAssert(!functionType->bound(), "");
	switch (functionType->kind())
	{
	case FunctionType::Kind::Declaration:
		solAssert(false, "Attempted to generate code for calling a function definition.");
		break;
	case FunctionType::Kind::Internal:
	{
		vector<string> args;
		for (unsigned i = 0; i < arguments.size(); ++i)
			if (functionType->takesArbitraryParameters())
				args.emplace_back(IRVariable(*arguments[i]).commaSeparatedList());
			else
				args.emplace_back(convert(*arguments[i], *parameterTypes[i]).commaSeparatedList());

		optional<FunctionDefinition const*> functionDef;
		if (auto memberAccess = dynamic_cast<MemberAccess const*>(&_functionCall.expression()))
		{
			solUnimplementedAssert(!functionType->bound(), "Internal calls to bound functions are not yet implemented for libraries and not allowed for contracts");

			functionDef = dynamic_cast<FunctionDefinition const*>(memberAccess->annotation().referencedDeclaration);
			if (functionDef.value() != nullptr)
				solAssert(functionType->declaration() == *memberAccess->annotation().referencedDeclaration, "");
			else
			{
				solAssert(dynamic_cast<VariableDeclaration const*>(memberAccess->annotation().referencedDeclaration), "");
				solAssert(!functionType->hasDeclaration(), "");
			}
		}
		else if (auto identifier = dynamic_cast<Identifier const*>(&_functionCall.expression()))
		{
			solAssert(!functionType->bound(), "");

			if (auto unresolvedFunctionDef = dynamic_cast<FunctionDefinition const*>(identifier->annotation().referencedDeclaration))
			{
				functionDef = &unresolvedFunctionDef->resolveVirtual(m_context.mostDerivedContract());
				solAssert(functionType->declaration() == *identifier->annotation().referencedDeclaration, "");
			}
			else
			{
				functionDef = nullptr;
				solAssert(dynamic_cast<VariableDeclaration const*>(identifier->annotation().referencedDeclaration), "");
				solAssert(!functionType->hasDeclaration(), "");
			}
		}
		else
			// Not a simple expression like x or A.x
			functionDef = nullptr;

		solAssert(functionDef.has_value(), "");
		solAssert(functionDef.value() == nullptr || functionDef.value()->isImplemented(), "");

		if (functionDef.value() != nullptr)
			define(_functionCall) <<
				m_context.enqueueFunctionForCodeGeneration(*functionDef.value()) <<
				"(" <<
				joinHumanReadable(args) <<
				")\n";
		else
			define(_functionCall) <<
				// NOTE: internalDispatch() takes care of adding the function to function generation queue
				m_context.internalDispatch(
					TupleType(functionType->parameterTypes()).sizeOnStack(),
					TupleType(functionType->returnParameterTypes()).sizeOnStack()
				) <<
				"(" <<
				IRVariable(_functionCall.expression()).part("functionIdentifier").name() <<
				joinHumanReadablePrefixed(args) <<
				")\n";
		break;
	}
	case FunctionType::Kind::External:
	case FunctionType::Kind::DelegateCall:
	case FunctionType::Kind::BareCall:
	case FunctionType::Kind::BareDelegateCall:
	case FunctionType::Kind::BareStaticCall:
		appendExternalFunctionCall(_functionCall, arguments);
		break;
	case FunctionType::Kind::BareCallCode:
		solAssert(false, "Callcode has been removed.");
	case FunctionType::Kind::Event:
	{
		auto const& event = dynamic_cast<EventDefinition const&>(functionType->declaration());
		TypePointers paramTypes = functionType->parameterTypes();
		ABIFunctions abi(m_context.evmVersion(), m_context.revertStrings(), m_context.functionCollector());

		vector<IRVariable> indexedArgs;
		string nonIndexedArgs;
		TypePointers nonIndexedArgTypes;
		TypePointers nonIndexedParamTypes;
		if (!event.isAnonymous())
			define(indexedArgs.emplace_back(m_context.newYulVariable(), *TypeProvider::uint256())) <<
				formatNumber(u256(h256::Arith(keccak256(functionType->externalSignature())))) << "\n";
		for (size_t i = 0; i < event.parameters().size(); ++i)
		{
			Expression const& arg = *arguments[i];
			if (event.parameters()[i]->isIndexed())
			{
				string value;
				if (auto const& referenceType = dynamic_cast<ReferenceType const*>(paramTypes[i]))
					define(indexedArgs.emplace_back(m_context.newYulVariable(), *TypeProvider::uint256())) <<
						m_utils.packedHashFunction({arg.annotation().type}, {referenceType}) <<
						"(" <<
						IRVariable(arg).commaSeparatedList() <<
						")";
				else
					indexedArgs.emplace_back(convert(arg, *paramTypes[i]));
			}
			else
			{
				string vars = IRVariable(arg).commaSeparatedList();
				if (!vars.empty())
					// In reverse because abi_encode expects it like that.
					nonIndexedArgs = ", " + move(vars) + nonIndexedArgs;
				nonIndexedArgTypes.push_back(arg.annotation().type);
				nonIndexedParamTypes.push_back(paramTypes[i]);
			}
		}
		solAssert(indexedArgs.size() <= 4, "Too many indexed arguments.");
		Whiskers templ(R"({
			let <pos> := <freeMemory>
			let <end> := <encode>(<pos> <nonIndexedArgs>)
			<log>(<pos>, sub(<end>, <pos>) <indexedArgs>)
		})");
		templ("pos", m_context.newYulVariable());
		templ("end", m_context.newYulVariable());
		templ("freeMemory", freeMemory());
		templ("encode", abi.tupleEncoder(nonIndexedArgTypes, nonIndexedParamTypes));
		templ("nonIndexedArgs", nonIndexedArgs);
		templ("log", "log" + to_string(indexedArgs.size()));
		templ("indexedArgs", joinHumanReadablePrefixed(indexedArgs | boost::adaptors::transformed([&](auto const& _arg) {
			return _arg.commaSeparatedList();
		})));
		m_code << templ.render();
		break;
	}
	case FunctionType::Kind::Assert:
	case FunctionType::Kind::Require:
	{
		solAssert(arguments.size() > 0, "Expected at least one parameter for require/assert");
		solAssert(arguments.size() <= 2, "Expected no more than two parameters for require/assert");

		Type const* messageArgumentType = arguments.size() > 1 ? arguments[1]->annotation().type : nullptr;
		string requireOrAssertFunction = m_utils.requireOrAssertFunction(
			functionType->kind() == FunctionType::Kind::Assert,
			messageArgumentType
		);

		m_code << move(requireOrAssertFunction) << "(" << IRVariable(*arguments[0]).name();
		if (messageArgumentType && messageArgumentType->sizeOnStack() > 0)
			m_code << ", " << IRVariable(*arguments[1]).commaSeparatedList();
		m_code << ")\n";

		break;
	}
	// Array creation using new
	case FunctionType::Kind::ObjectCreation:
	{
		ArrayType const& arrayType = dynamic_cast<ArrayType const&>(*_functionCall.annotation().type);
		solAssert(arguments.size() == 1, "");

		IRVariable value = convert(*arguments[0], *TypeProvider::uint256());
		define(_functionCall) <<
			m_utils.allocateAndInitializeMemoryArrayFunction(arrayType) <<
			"(" <<
			value.commaSeparatedList() <<
			")\n";
		break;
	}
	case FunctionType::Kind::KECCAK256:
	{
		solAssert(arguments.size() == 1, "");

		ArrayType const* arrayType = TypeProvider::bytesMemory();
		auto array = convert(*arguments[0], *arrayType);

		define(_functionCall) <<
			"keccak256(" <<
			m_utils.arrayDataAreaFunction(*arrayType) <<
			"(" <<
			array.commaSeparatedList() <<
			"), " <<
			m_utils.arrayLengthFunction(*arrayType) <<
			"(" <<
			array.commaSeparatedList() <<
			"))\n";
		break;
	}
	case FunctionType::Kind::ECRecover:
	case FunctionType::Kind::SHA256:
	case FunctionType::Kind::RIPEMD160:
	{
		solAssert(!_functionCall.annotation().tryCall, "");
		appendExternalFunctionCall(_functionCall, arguments);
		break;
	}
	case FunctionType::Kind::ArrayPop:
	{
		auto const& memberAccessExpression = dynamic_cast<MemberAccess const&>(_functionCall.expression()).expression();
		ArrayType const& arrayType = dynamic_cast<ArrayType const&>(*memberAccessExpression.annotation().type);
		define(_functionCall) <<
			m_utils.storageArrayPopFunction(arrayType) <<
			"(" <<
			IRVariable(_functionCall.expression()).commaSeparatedList() <<
			")\n";
		break;
	}
	case FunctionType::Kind::ArrayPush:
	{
		auto const& memberAccessExpression = dynamic_cast<MemberAccess const&>(_functionCall.expression()).expression();
		ArrayType const& arrayType = dynamic_cast<ArrayType const&>(*memberAccessExpression.annotation().type);
		if (arguments.empty())
		{
			auto slotName = m_context.newYulVariable();
			auto offsetName = m_context.newYulVariable();
			m_code << "let " << slotName << ", " << offsetName << " := " <<
				m_utils.storageArrayPushZeroFunction(arrayType) <<
				"(" << IRVariable(_functionCall.expression()).commaSeparatedList() << ")\n";
			setLValue(_functionCall, IRLValue{
				*arrayType.baseType(),
				IRLValue::Storage{
					slotName,
					offsetName,
				}
			});
		}
		else
		{
			IRVariable argument = convert(*arguments.front(), *arrayType.baseType());
			m_code <<
				m_utils.storageArrayPushFunction(arrayType) <<
				"(" <<
				IRVariable(_functionCall.expression()).commaSeparatedList() <<
				", " <<
				argument.commaSeparatedList() <<
				")\n";
		}
		break;
	}
	case FunctionType::Kind::MetaType:
	{
		break;
	}
	case FunctionType::Kind::GasLeft:
	{
		define(_functionCall) << "gas()\n";
		break;
	}
	case FunctionType::Kind::Selfdestruct:
	{
		solAssert(arguments.size() == 1, "");
		define(_functionCall) << "selfdestruct(" << expressionAsType(*arguments.front(), *parameterTypes.front()) << ")\n";
		break;
	}
	case FunctionType::Kind::Log0:
	case FunctionType::Kind::Log1:
	case FunctionType::Kind::Log2:
	case FunctionType::Kind::Log3:
	case FunctionType::Kind::Log4:
	{
		unsigned logNumber = int(functionType->kind()) - int(FunctionType::Kind::Log0);
		solAssert(arguments.size() == logNumber + 1, "");
		ABIFunctions abi(m_context.evmVersion(), m_context.revertStrings(), m_context.functionCollector());
		string indexedArgs;
		for (unsigned arg = 0; arg < logNumber; ++arg)
			indexedArgs += ", " + expressionAsType(*arguments[arg + 1], *(parameterTypes[arg + 1]));
		Whiskers templ(R"({
			let <pos> := <freeMemory>
			let <end> := <encode>(<pos>, <nonIndexedArgs>)
			<log>(<pos>, sub(<end>, <pos>) <indexedArgs>)
		})");
		templ("pos", m_context.newYulVariable());
		templ("end", m_context.newYulVariable());
		templ("freeMemory", freeMemory());
		templ("encode", abi.tupleEncoder({arguments.front()->annotation().type}, {parameterTypes.front()}));
		templ("nonIndexedArgs", IRVariable(*arguments.front()).commaSeparatedList());
		templ("log", "log" + to_string(logNumber));
		templ("indexedArgs", indexedArgs);
		m_code << templ.render();

		break;
	}
	case FunctionType::Kind::Creation:
	{
		solAssert(!functionType->gasSet(), "Gas limit set for contract creation.");
		solAssert(
			functionType->returnParameterTypes().size() == 1,
			"Constructor should return only one type"
		);

		TypePointers argumentTypes;
		string constructorParams;
		for (ASTPointer<Expression const> const& arg: arguments)
		{
			argumentTypes.push_back(arg->annotation().type);
			constructorParams += ", " + IRVariable{*arg}.commaSeparatedList();
		}

		ContractDefinition const* contract =
			&dynamic_cast<ContractType const&>(*functionType->returnParameterTypes().front()).contractDefinition();
		m_context.subObjectsCreated().insert(contract);

		Whiskers t(R"(
			let <memPos> := <allocateTemporaryMemory>()
			let <memEnd> := add(<memPos>, datasize("<object>"))
			if or(gt(<memEnd>, 0xffffffffffffffff), lt(<memEnd>, <memPos>)) { revert(0, 0) }
			datacopy(<memPos>, dataoffset("<object>"), datasize("<object>"))
			<memEnd> := <abiEncode>(<memEnd><constructorParams>)
			<?saltSet>
				let <retVars> := create2(<value>, <memPos>, sub(<memEnd>, <memPos>), <salt>)
			<!saltSet>
				let <retVars> := create(<value>, <memPos>, sub(<memEnd>, <memPos>))
			</saltSet>
			<releaseTemporaryMemory>()
		)");
		t("memPos", m_context.newYulVariable());
		t("memEnd", m_context.newYulVariable());
		t("allocateTemporaryMemory", m_utils.allocationTemporaryMemoryFunction());
		t("releaseTemporaryMemory", m_utils.releaseTemporaryMemoryFunction());
		t("object", m_context.creationObjectName(*contract));
		t("abiEncode",
			m_context.abiFunctions().tupleEncoder(argumentTypes, functionType->parameterTypes(),false)
		);
		t("constructorParams", constructorParams);
		t("value", functionType->valueSet() ? IRVariable(_functionCall.expression()).part("value").name() : "0");
		t("saltSet", functionType->saltSet());
		if (functionType->saltSet())
			t("salt", IRVariable(_functionCall.expression()).part("salt").name());
		t("retVars", IRVariable(_functionCall).commaSeparatedList());
		m_code << t.render();

		break;
	}
	default:
		solUnimplemented("FunctionKind " + toString(static_cast<int>(functionType->kind())) + " not yet implemented");
	}
}

void IRGeneratorForStatements::endVisit(FunctionCallOptions const& _options)
{
	FunctionType const& previousType = dynamic_cast<FunctionType const&>(*_options.expression().annotation().type);

	solUnimplementedAssert(!previousType.bound(), "");

	// Copy over existing values.
	for (auto const& item: previousType.stackItems())
		define(IRVariable(_options).part(get<0>(item)), IRVariable(_options.expression()).part(get<0>(item)));

	for (size_t i = 0; i < _options.names().size(); ++i)
	{
		string const& name = *_options.names()[i];
		solAssert(name == "salt" || name == "gas" || name == "value", "");

		define(IRVariable(_options).part(name), *_options.options()[i]);
	}
}

void IRGeneratorForStatements::endVisit(MemberAccess const& _memberAccess)
{
	ASTString const& member = _memberAccess.memberName();
	if (auto funType = dynamic_cast<FunctionType const*>(_memberAccess.annotation().type))
		if (funType->bound())
		{
			solUnimplementedAssert(false, "");
		}

	switch (_memberAccess.expression().annotation().type->category())
	{
	case Type::Category::Contract:
	{
		ContractType const& type = dynamic_cast<ContractType const&>(*_memberAccess.expression().annotation().type);
		if (type.isSuper())
		{
			solUnimplementedAssert(false, "");
		}
		// ordinary contract type
		else if (Declaration const* declaration = _memberAccess.annotation().referencedDeclaration)
		{
			u256 identifier;
			if (auto const* variable = dynamic_cast<VariableDeclaration const*>(declaration))
				identifier = FunctionType(*variable).externalIdentifier();
			else if (auto const* function = dynamic_cast<FunctionDefinition const*>(declaration))
				identifier = FunctionType(*function).externalIdentifier();
			else
				solAssert(false, "Contract member is neither variable nor function.");

			define(IRVariable(_memberAccess).part("address"), _memberAccess.expression());
			define(IRVariable(_memberAccess).part("functionIdentifier")) << formatNumber(identifier) << "\n";
		}
		else
			solAssert(false, "Invalid member access in contract");
		break;
	}
	case Type::Category::Integer:
	{
		solAssert(false, "Invalid member access to integer");
		break;
	}
	case Type::Category::Address:
	{
		if (member == "balance")
			define(_memberAccess) <<
				"balance(" <<
				expressionAsType(_memberAccess.expression(), *TypeProvider::address()) <<
				")\n";
		else if (set<string>{"send", "transfer"}.count(member))
		{
			solAssert(dynamic_cast<AddressType const&>(*_memberAccess.expression().annotation().type).stateMutability() == StateMutability::Payable, "");
			define(IRVariable{_memberAccess}.part("address"), _memberAccess.expression());
		}
		else if (set<string>{"call", "callcode", "delegatecall", "staticcall"}.count(member))
			define(IRVariable{_memberAccess}.part("address"), _memberAccess.expression());
		else
			solAssert(false, "Invalid member access to address");
		break;
	}
	case Type::Category::Function:
		if (member == "selector")
		{
			FunctionType const& functionType = dynamic_cast<FunctionType const&>(
				*_memberAccess.expression().annotation().type
			);
			if (functionType.kind() == FunctionType::Kind::External)
				define(IRVariable{_memberAccess}, IRVariable(_memberAccess.expression()).part("functionIdentifier"));
			else if (functionType.kind() == FunctionType::Kind::Declaration)
			{
				solAssert(functionType.hasDeclaration(), "");
				define(IRVariable{_memberAccess}) << formatNumber(functionType.externalIdentifier() << 224) << "\n";
			}
			else
				solAssert(false, "Invalid use of .selector");
		}
		else if (member == "address")
		{
			solUnimplementedAssert(
				dynamic_cast<FunctionType const&>(*_memberAccess.expression().annotation().type).kind() ==
				FunctionType::Kind::External, ""
			);
			define(IRVariable{_memberAccess}, IRVariable(_memberAccess.expression()).part("address"));
		}
		else
			solAssert(
				!!_memberAccess.expression().annotation().type->memberType(member),
				"Invalid member access to function."
			);
		break;
	case Type::Category::Magic:
		// we can ignore the kind of magic and only look at the name of the member
		if (member == "coinbase")
			define(_memberAccess) << "coinbase()\n";
		else if (member == "timestamp")
			define(_memberAccess) << "timestamp()\n";
		else if (member == "difficulty")
			define(_memberAccess) << "difficulty()\n";
		else if (member == "number")
			define(_memberAccess) << "number()\n";
		else if (member == "gaslimit")
			define(_memberAccess) << "gaslimit()\n";
		else if (member == "sender")
			define(_memberAccess) << "caller()\n";
		else if (member == "value")
			define(_memberAccess) << "callvalue()\n";
		else if (member == "origin")
			define(_memberAccess) << "origin()\n";
		else if (member == "gasprice")
			define(_memberAccess) << "gasprice()\n";
		else if (member == "data")
		{
			IRVariable var(_memberAccess);
			declare(var);
			define(var.part("offset")) << "0\n";
			define(var.part("length")) << "calldatasize()\n";
		}
		else if (member == "sig")
			define(_memberAccess) <<
				"and(calldataload(0), " <<
				formatNumber(u256(0xffffffff) << (256 - 32)) <<
				")\n";
		else if (member == "gas")
			solAssert(false, "Gas has been removed.");
		else if (member == "blockhash")
			solAssert(false, "Blockhash has been removed.");
		else if (member == "creationCode" || member == "runtimeCode")
		{
			solUnimplementedAssert(false, "");
		}
		else if (member == "name")
		{
			solUnimplementedAssert(false, "");
		}
		else if (member == "interfaceId")
		{
			TypePointer arg = dynamic_cast<MagicType const&>(*_memberAccess.expression().annotation().type).typeArgument();
			ContractDefinition const& contract = dynamic_cast<ContractType const&>(*arg).contractDefinition();
			uint64_t result{0};
			for (auto const& function: contract.interfaceFunctionList(false))
				result ^= fromBigEndian<uint64_t>(function.first.ref());
			define(_memberAccess) << formatNumber(u256{result} << (256 - 32)) << "\n";
		}
		else if (set<string>{"encode", "encodePacked", "encodeWithSelector", "encodeWithSignature", "decode"}.count(member))
		{
			// no-op
		}
		else
			solAssert(false, "Unknown magic member.");
		break;
	case Type::Category::Struct:
	{
		solUnimplementedAssert(false, "");
	}
	case Type::Category::Enum:
	{
		EnumType const& type = dynamic_cast<EnumType const&>(*_memberAccess.expression().annotation().type);
		define(_memberAccess) << to_string(type.memberValue(_memberAccess.memberName())) << "\n";
		break;
	}
	case Type::Category::Array:
	{
		auto const& type = dynamic_cast<ArrayType const&>(*_memberAccess.expression().annotation().type);

		if (member == "length")
		{
			if (!type.isDynamicallySized())
				define(_memberAccess) << type.length() << "\n";
			else
				switch (type.location())
				{
					case DataLocation::CallData:
						define(_memberAccess, IRVariable(_memberAccess.expression()).part("length"));
						break;
					case DataLocation::Storage:
					{
						define(_memberAccess) <<
							m_utils.arrayLengthFunction(type) <<
							"(" <<
							IRVariable(_memberAccess.expression()).commaSeparatedList() <<
							")\n";
						break;
					}
					case DataLocation::Memory:
						define(_memberAccess) <<
							"mload(" <<
							IRVariable(_memberAccess.expression()).commaSeparatedList() <<
							")\n";
						break;
				}
		}
		else if (member == "pop" || member == "push")
		{
			solAssert(type.location() == DataLocation::Storage, "");
			define(IRVariable{_memberAccess}.part("slot"), IRVariable{_memberAccess.expression()}.part("slot"));
		}
		else
			solAssert(false, "Invalid array member access.");

		break;
	}
	case Type::Category::FixedBytes:
	{
		auto const& type = dynamic_cast<FixedBytesType const&>(*_memberAccess.expression().annotation().type);
		if (member == "length")
			define(_memberAccess) << to_string(type.numBytes()) << "\n";
		else
			solAssert(false, "Illegal fixed bytes member.");
		break;
	}
	case Type::Category::TypeType:
	{
		Type const& actualType = *dynamic_cast<TypeType const&>(
			*_memberAccess.expression().annotation().type
		).actualType();

		if (actualType.category() == Type::Category::Contract)
		{
			if (auto const* variable = dynamic_cast<VariableDeclaration const*>(_memberAccess.annotation().referencedDeclaration))
				handleVariableReference(*variable, _memberAccess);
			else if (auto const* funType = dynamic_cast<FunctionType const*>(_memberAccess.annotation().type))
			{
				switch (funType->kind())
				{
				case FunctionType::Kind::Declaration:
					break;
				case FunctionType::Kind::Internal:
					if (auto const* function = dynamic_cast<FunctionDefinition const*>(_memberAccess.annotation().referencedDeclaration))
						define(_memberAccess) << to_string(function->id()) << "\n";
					else
						solAssert(false, "Function not found in member access");
					break;
				case FunctionType::Kind::Event:
					solAssert(
						dynamic_cast<EventDefinition const*>(_memberAccess.annotation().referencedDeclaration),
						"Event not found"
					);
					// the call will do the resolving
					break;
				case FunctionType::Kind::DelegateCall:
					define(IRVariable(_memberAccess).part("address"), _memberAccess.expression());
					define(IRVariable(_memberAccess).part("functionIdentifier")) << formatNumber(funType->externalIdentifier()) << "\n";
					break;
				case FunctionType::Kind::External:
				case FunctionType::Kind::Creation:
				case FunctionType::Kind::Send:
				case FunctionType::Kind::BareCall:
				case FunctionType::Kind::BareCallCode:
				case FunctionType::Kind::BareDelegateCall:
				case FunctionType::Kind::BareStaticCall:
				case FunctionType::Kind::Transfer:
				case FunctionType::Kind::Log0:
				case FunctionType::Kind::Log1:
				case FunctionType::Kind::Log2:
				case FunctionType::Kind::Log3:
				case FunctionType::Kind::Log4:
				case FunctionType::Kind::ECRecover:
				case FunctionType::Kind::SHA256:
				case FunctionType::Kind::RIPEMD160:
				default:
					solAssert(false, "unsupported member function");
				}
			}
			else if (dynamic_cast<TypeType const*>(_memberAccess.annotation().type))
			{
				// no-op
			}
			else
				// The old code generator had a generic "else" case here
				// without any specific code being generated,
				// but it would still be better to have an exhaustive list.
				solAssert(false, "");
		}
		else if (EnumType const* enumType = dynamic_cast<EnumType const*>(&actualType))
			define(_memberAccess) << to_string(enumType->memberValue(_memberAccess.memberName())) << "\n";
		else
			// The old code generator had a generic "else" case here
			// without any specific code being generated,
			// but it would still be better to have an exhaustive list.
			solAssert(false, "");
		break;
	}
	default:
		solAssert(false, "Member access to unknown type.");
	}
}

bool IRGeneratorForStatements::visit(InlineAssembly const& _inlineAsm)
{
	CopyTranslate bodyCopier{_inlineAsm.dialect(), m_context, _inlineAsm.annotation().externalReferences};

	yul::Statement modified = bodyCopier(_inlineAsm.operations());

	solAssert(holds_alternative<yul::Block>(modified), "");

	// Do not provide dialect so that we get the full type information.
	m_code << yul::AsmPrinter()(std::get<yul::Block>(modified)) << "\n";
	return false;
}


void IRGeneratorForStatements::endVisit(IndexAccess const& _indexAccess)
{
	Type const& baseType = *_indexAccess.baseExpression().annotation().type;

	if (baseType.category() == Type::Category::Mapping)
	{
		solAssert(_indexAccess.indexExpression(), "Index expression expected.");

		MappingType const& mappingType = dynamic_cast<MappingType const&>(baseType);
		Type const& keyType = *_indexAccess.indexExpression()->annotation().type;
		solAssert(keyType.sizeOnStack() <= 1, "");

		string slot = m_context.newYulVariable();
		Whiskers templ("let <slot> := <indexAccess>(<base> <key>)\n");
		templ("slot", slot);
		templ("indexAccess", m_utils.mappingIndexAccessFunction(mappingType, keyType));
		templ("base", IRVariable(_indexAccess.baseExpression()).commaSeparatedList());
		if (keyType.sizeOnStack() == 0)
			templ("key", "");
		else
			templ("key", ", " + IRVariable(*_indexAccess.indexExpression()).commaSeparatedList());
		m_code << templ.render();
		setLValue(_indexAccess, IRLValue{
			*_indexAccess.annotation().type,
			IRLValue::Storage{
				slot,
				0u
			}
		});
	}
	else if (baseType.category() == Type::Category::Array || baseType.category() == Type::Category::ArraySlice)
	{
		ArrayType const& arrayType =
			baseType.category() == Type::Category::Array ?
			dynamic_cast<ArrayType const&>(baseType) :
			dynamic_cast<ArraySliceType const&>(baseType).arrayType();

		if (baseType.category() == Type::Category::ArraySlice)
			solAssert(arrayType.dataStoredIn(DataLocation::CallData) && arrayType.isDynamicallySized(), "");

		solAssert(_indexAccess.indexExpression(), "Index expression expected.");

		switch (arrayType.location())
		{
			case DataLocation::Storage:
			{
				string slot = m_context.newYulVariable();
				string offset = m_context.newYulVariable();

				m_code << Whiskers(R"(
					let <slot>, <offset> := <indexFunc>(<array>, <index>)
				)")
				("slot", slot)
				("offset", offset)
				("indexFunc", m_utils.storageArrayIndexAccessFunction(arrayType))
				("array", IRVariable(_indexAccess.baseExpression()).part("slot").name())
				("index", IRVariable(*_indexAccess.indexExpression()).name())
				.render();

				setLValue(_indexAccess, IRLValue{
					*_indexAccess.annotation().type,
					IRLValue::Storage{slot, offset}
				});

				break;
			}
			case DataLocation::Memory:
			{
				string const memAddress =
					m_utils.memoryArrayIndexAccessFunction(arrayType) +
					"(" +
					IRVariable(_indexAccess.baseExpression()).part("mpos").name() +
					", " +
					expressionAsType(*_indexAccess.indexExpression(), *TypeProvider::uint256()) +
					")";

				setLValue(_indexAccess, IRLValue{
					*arrayType.baseType(),
					IRLValue::Memory{memAddress}
				});
				break;
			}
			case DataLocation::CallData:
			{
				IRVariable var(m_context.newYulVariable(), *arrayType.baseType());
				define(var) <<
					m_utils.calldataArrayIndexAccessFunction(arrayType) <<
					"(" <<
					IRVariable(_indexAccess.baseExpression()).commaSeparatedList() <<
					", " <<
					expressionAsType(*_indexAccess.indexExpression(), *TypeProvider::uint256()) <<
					")\n";
				if (arrayType.isByteArray())
					define(_indexAccess) <<
						m_utils.cleanupFunction(*arrayType.baseType()) <<
						"(calldataload(" <<
						var.name() <<
						"))\n";
				else if (arrayType.baseType()->isValueType())
					define(_indexAccess) <<
						m_utils.readFromCalldata(*arrayType.baseType()) <<
						"(" <<
						var.commaSeparatedList() <<
						")\n";
				else
					define(_indexAccess, var);
				break;
			}
		}
	}
	else if (baseType.category() == Type::Category::FixedBytes)
		solUnimplementedAssert(false, "");
	else if (baseType.category() == Type::Category::TypeType)
	{
		solAssert(baseType.sizeOnStack() == 0, "");
		solAssert(_indexAccess.annotation().type->sizeOnStack() == 0, "");
		// no-op - this seems to be a lone array type (`structType[];`)
	}
	else
		solAssert(false, "Index access only allowed for mappings or arrays.");
}

void IRGeneratorForStatements::endVisit(IndexRangeAccess const& _indexRangeAccess)
{
	Type const& baseType = *_indexRangeAccess.baseExpression().annotation().type;
	solAssert(
		baseType.category() == Type::Category::Array || baseType.category() == Type::Category::ArraySlice,
		"Index range accesses is available only on arrays and array slices."
	);

	ArrayType const& arrayType =
		baseType.category() == Type::Category::Array ?
		dynamic_cast<ArrayType const &>(baseType) :
		dynamic_cast<ArraySliceType const &>(baseType).arrayType();

	switch (arrayType.location())
	{
		case DataLocation::CallData:
		{
			solAssert(baseType.isDynamicallySized(), "");
			IRVariable sliceStart{m_context.newYulVariable(), *TypeProvider::uint256()};
			if (_indexRangeAccess.startExpression())
				define(sliceStart, IRVariable{*_indexRangeAccess.startExpression()});
			else
				define(sliceStart) << u256(0) << "\n";

			IRVariable sliceEnd{
				m_context.newYulVariable(),
				*TypeProvider::uint256()
			};
			if (_indexRangeAccess.endExpression())
				define(sliceEnd, IRVariable{*_indexRangeAccess.endExpression()});
			else
				define(sliceEnd, IRVariable{_indexRangeAccess.baseExpression()}.part("length"));

			IRVariable range{_indexRangeAccess};
			define(range) <<
				m_utils.calldataArrayIndexRangeAccess(arrayType) << "(" <<
				IRVariable{_indexRangeAccess.baseExpression()}.commaSeparatedList() << ", " <<
				sliceStart.name() << ", " <<
				sliceEnd.name() << ")\n";
			break;
		}
		default:
			solUnimplementedAssert(false, "Index range accesses is implemented only on calldata arrays.");
	}
}

void IRGeneratorForStatements::endVisit(Identifier const& _identifier)
{
	Declaration const* declaration = _identifier.annotation().referencedDeclaration;
	if (MagicVariableDeclaration const* magicVar = dynamic_cast<MagicVariableDeclaration const*>(declaration))
	{
		switch (magicVar->type()->category())
		{
		case Type::Category::Contract:
			if (dynamic_cast<ContractType const&>(*magicVar->type()).isSuper())
				solAssert(_identifier.name() == "super", "");
			else
			{
				solAssert(_identifier.name() == "this", "");
				define(_identifier) << "address()\n";
			}
			break;
		case Type::Category::Integer:
			solAssert(_identifier.name() == "now", "");
			define(_identifier) << "timestamp()\n";
			break;
		default:
			break;
		}
		return;
	}
	else if (FunctionDefinition const* functionDef = dynamic_cast<FunctionDefinition const*>(declaration))
		define(_identifier) << to_string(functionDef->resolveVirtual(m_context.mostDerivedContract()).id()) << "\n";
	else if (VariableDeclaration const* varDecl = dynamic_cast<VariableDeclaration const*>(declaration))
		handleVariableReference(*varDecl, _identifier);
	else if (dynamic_cast<ContractDefinition const*>(declaration))
	{
		// no-op
	}
	else if (dynamic_cast<EventDefinition const*>(declaration))
	{
		// no-op
	}
	else if (dynamic_cast<EnumDefinition const*>(declaration))
	{
		// no-op
	}
	else if (dynamic_cast<StructDefinition const*>(declaration))
	{
		// no-op
	}
	else
	{
		solAssert(false, "Identifier type not expected in expression context.");
	}
}

bool IRGeneratorForStatements::visit(Literal const& _literal)
{
	Type const& literalType = type(_literal);

	switch (literalType.category())
	{
	case Type::Category::RationalNumber:
	case Type::Category::Bool:
	case Type::Category::Address:
		define(_literal) << toCompactHexWithPrefix(literalType.literalValue(&_literal)) << "\n";
		break;
	case Type::Category::StringLiteral:
		break; // will be done during conversion
	default:
		solUnimplemented("Only integer, boolean and string literals implemented for now.");
	}
	return false;
}

void IRGeneratorForStatements::handleVariableReference(
	VariableDeclaration const& _variable,
	Expression const& _referencingExpression
)
{
	// TODO for the constant case, we have to be careful:
	// If the value is visited twice, `defineExpression` is called twice on
	// the same expression.
	solUnimplementedAssert(!_variable.isConstant(), "");
	solUnimplementedAssert(!_variable.immutable(), "");
	if (m_context.isLocalVariable(_variable))
		setLValue(_referencingExpression, IRLValue{
			*_variable.annotation().type,
			IRLValue::Stack{m_context.localVariable(_variable)}
		});
	else if (m_context.isStateVariable(_variable))
		setLValue(_referencingExpression, IRLValue{
			*_variable.annotation().type,
			IRLValue::Storage{
				toCompactHexWithPrefix(m_context.storageLocationOfVariable(_variable).first),
				m_context.storageLocationOfVariable(_variable).second
			}
		});
	else
		solAssert(false, "Invalid variable kind.");
}

void IRGeneratorForStatements::appendExternalFunctionCall(
	FunctionCall const& _functionCall,
	vector<ASTPointer<Expression const>> const& _arguments
)
{
	FunctionType const& funType = dynamic_cast<FunctionType const&>(type(_functionCall.expression()));
	solAssert(
		funType.takesArbitraryParameters() ||
		_arguments.size() == funType.parameterTypes().size(), ""
	);
	solUnimplementedAssert(!funType.bound(), "");
	FunctionType::Kind const funKind = funType.kind();

	solAssert(funKind != FunctionType::Kind::BareStaticCall || m_context.evmVersion().hasStaticCall(), "");
	solAssert(funKind != FunctionType::Kind::BareCallCode, "Callcode has been removed.");

	bool const isDelegateCall = funKind == FunctionType::Kind::BareDelegateCall || funKind == FunctionType::Kind::DelegateCall;
	bool const useStaticCall = funKind == FunctionType::Kind::BareStaticCall || (funType.stateMutability() <= StateMutability::View && m_context.evmVersion().hasStaticCall());

	ReturnInfo const returnInfo{m_context.evmVersion(), funType};

	TypePointers argumentTypes;
	vector<string> argumentStrings;
	for (auto const& arg: _arguments)
	{
		argumentTypes.emplace_back(&type(*arg));
		if (IRVariable(*arg).type().sizeOnStack() > 0)
			argumentStrings.emplace_back(IRVariable(*arg).commaSeparatedList());
	}
	string argumentString = argumentStrings.empty() ? ""s : (", " + joinHumanReadable(argumentStrings));

	solUnimplementedAssert(funKind != FunctionType::Kind::ECRecover, "");

	if (!m_context.evmVersion().canOverchargeGasForCall())
	{
		// Touch the end of the output area so that we do not pay for memory resize during the call
		// (which we would have to subtract from the gas left)
		// We could also just use MLOAD; POP right before the gas calculation, but the optimizer
		// would remove that, so we use MSTORE here.
		if (!funType.gasSet() && returnInfo.estimatedReturnSize > 0)
			m_code << "mstore(add(" << freeMemory() << ", " << to_string(returnInfo.estimatedReturnSize) << "), 0)\n";
	}

	ABIFunctions abi(m_context.evmVersion(), m_context.revertStrings(), m_context.functionCollector());

	Whiskers templ(R"(
		<?checkExistence>
			if iszero(extcodesize(<address>)) { revert(0, 0) }
		</checkExistence>

		// storage for arguments and returned data
		let <pos> := <freeMemory>
		<?bareCall>
		<!bareCall>
			mstore(<pos>, <shl28>(<funId>))
		</bareCall>
		let <end> := <encodeArgs>(
			<?bareCall>
				<pos>
			<!bareCall>
				add(<pos>, 4)
			</bareCall>
			<argumentString>
		)

		let <success> := <call>(<gas>, <address>, <?hasValue> <value>, </hasValue> <pos>, sub(<end>, <pos>), <pos>, <reservedReturnSize>)
		<?noTryCall>
			if iszero(<success>) { <forwardingRevert>() }
		</noTryCall>
		<?hasRetVars> let <retVars> </hasRetVars>
		if <success> {
			<?dynamicReturnSize>
				// copy dynamic return data out
				returndatacopy(<pos>, 0, returndatasize())
			</dynamicReturnSize>

			// update freeMemoryPointer according to dynamic return size
			mstore(<freeMemoryPointer>, add(<pos>, <roundUp>(<returnSize>)))

			// decode return parameters from external try-call into retVars
			<?hasRetVars> <retVars> := </hasRetVars> <abiDecode>(<pos>, add(<pos>, <returnSize>))
		}
	)");
	templ("pos", m_context.newYulVariable());
	templ("end", m_context.newYulVariable());
	templ("bareCall", funType.isBareCall());
	if (_functionCall.annotation().tryCall)
		templ("success", m_context.trySuccessConditionVariable(_functionCall));
	else
		templ("success", m_context.newYulVariable());
	templ("freeMemory", freeMemory());
	templ("shl28", m_utils.shiftLeftFunction(8 * (32 - 4)));

	if (!funType.isBareCall())
		templ("funId", IRVariable(_functionCall.expression()).part("functionIdentifier").name());

	if (funKind == FunctionType::Kind::ECRecover)
		templ("address", "1");
	else if (funKind == FunctionType::Kind::SHA256)
		templ("address", "2");
	else if (funKind == FunctionType::Kind::RIPEMD160)
		templ("address", "3");
	else
		templ("address", IRVariable(_functionCall.expression()).part("address").name());

	// Always use the actual return length, and not our calculated expected length, if returndatacopy is supported.
	// This ensures it can catch badly formatted input from external calls.
	if (m_context.evmVersion().supportsReturndata())
		templ("returnSize", "returndatasize()");
	else
		templ("returnSize", to_string(returnInfo.estimatedReturnSize));

	templ("reservedReturnSize", returnInfo.dynamicReturnSize ? "0" : to_string(returnInfo.estimatedReturnSize));

	string const retVars = IRVariable(_functionCall).commaSeparatedList();
	templ("retVars", retVars);
	templ("hasRetVars", !retVars.empty());
	solAssert(retVars.empty() == returnInfo.returnTypes.empty(), "");

	templ("roundUp", m_utils.roundUpFunction());
	templ("abiDecode", abi.tupleDecoder(returnInfo.returnTypes, true));
	templ("dynamicReturnSize", returnInfo.dynamicReturnSize);
	templ("freeMemoryPointer", to_string(CompilerUtils::freeMemoryPointer));

	templ("noTryCall", !_functionCall.annotation().tryCall);

	// If the function takes arbitrary parameters or is a bare call, copy dynamic length data in place.
	// Move arguments to memory, will not update the free memory pointer (but will update the memory
	// pointer on the stack).
	bool encodeInPlace = funType.takesArbitraryParameters() || funType.isBareCall();
	if (funType.kind() == FunctionType::Kind::ECRecover)
		// This would be the only combination of padding and in-place encoding,
		// but all parameters of ecrecover are value types anyway.
		encodeInPlace = false;
	bool encodeForLibraryCall = funKind == FunctionType::Kind::DelegateCall;

	solUnimplementedAssert(encodeInPlace == !funType.padArguments(), "");
	if (encodeInPlace)
	{
		solUnimplementedAssert(!encodeForLibraryCall, "");
		templ("encodeArgs", abi.tupleEncoderPacked(argumentTypes, funType.parameterTypes()));
	}
	else
		templ("encodeArgs", abi.tupleEncoder(argumentTypes, funType.parameterTypes(), encodeForLibraryCall));
	templ("argumentString", argumentString);

	// Output data will replace input data, unless we have ECRecover (then, output
	// area will be 32 bytes just before input area).
	solUnimplementedAssert(funKind != FunctionType::Kind::ECRecover, "");

	solAssert(!isDelegateCall || !funType.valueSet(), "Value set for delegatecall");
	solAssert(!useStaticCall || !funType.valueSet(), "Value set for staticcall");

	templ("hasValue", !isDelegateCall && !useStaticCall);
	templ("value", funType.valueSet() ? IRVariable(_functionCall.expression()).part("value").name() : "0");

	// Check that the target contract exists (has code) for non-low-level calls.
	bool checkExistence = (funKind == FunctionType::Kind::External || funKind == FunctionType::Kind::DelegateCall);
	templ("checkExistence", checkExistence);

	if (funType.gasSet())
		templ("gas", IRVariable(_functionCall.expression()).part("gas").name());
	else if (m_context.evmVersion().canOverchargeGasForCall())
		// Send all gas (requires tangerine whistle EVM)
		templ("gas", "gas()");
	else
	{
		// send all gas except the amount needed to execute "SUB" and "CALL"
		// @todo this retains too much gas for now, needs to be fine-tuned.
		u256 gasNeededByCaller = evmasm::GasCosts::callGas(m_context.evmVersion()) + 10;
		if (funType.valueSet())
			gasNeededByCaller += evmasm::GasCosts::callValueTransferGas;
		if (!checkExistence)
			gasNeededByCaller += evmasm::GasCosts::callNewAccountGas; // we never know
		templ("gas", "sub(gas(), " + formatNumber(gasNeededByCaller) + ")");
	}
	// Order is important here, STATICCALL might overlap with DELEGATECALL.
	if (isDelegateCall)
		templ("call", "delegatecall");
	else if (useStaticCall)
		templ("call", "staticcall");
	else
		templ("call", "call");

	templ("forwardingRevert", m_utils.forwardingRevertFunction());

	solUnimplementedAssert(funKind != FunctionType::Kind::RIPEMD160, "");
	solUnimplementedAssert(funKind != FunctionType::Kind::ECRecover, "");

	m_code << templ.render();
}

string IRGeneratorForStatements::freeMemory()
{
	return "mload(" + to_string(CompilerUtils::freeMemoryPointer) + ")";
}

IRVariable IRGeneratorForStatements::convert(IRVariable const& _from, Type const& _to)
{
	if (_from.type() == _to)
		return _from;
	else
	{
		IRVariable converted(m_context.newYulVariable(), _to);
		define(converted, _from);
		return converted;
	}
}

std::string IRGeneratorForStatements::expressionAsType(Expression const& _expression, Type const& _to)
{
	IRVariable from(_expression);
	if (from.type() == _to)
		return from.commaSeparatedList();
	else
		return m_utils.conversionFunction(from.type(), _to) + "(" + from.commaSeparatedList() + ")";
}

std::ostream& IRGeneratorForStatements::define(IRVariable const& _var)
{
	if (_var.type().sizeOnStack() > 0)
		m_code << "let " << _var.commaSeparatedList() << " := ";
	return m_code;
}

void IRGeneratorForStatements::declare(IRVariable const& _var)
{
	if (_var.type().sizeOnStack() > 0)
		m_code << "let " << _var.commaSeparatedList() << "\n";
}

void IRGeneratorForStatements::declareAssign(IRVariable const& _lhs, IRVariable const& _rhs, bool _declare)
{
	string output;
	if (_lhs.type() == _rhs.type())
		for (auto const& [stackItemName, stackItemType]: _lhs.type().stackItems())
			if (stackItemType)
				declareAssign(_lhs.part(stackItemName), _rhs.part(stackItemName), _declare);
			else
				m_code << (_declare ? "let ": "") << _lhs.part(stackItemName).name() << " := " << _rhs.part(stackItemName).name() << "\n";
	else
	{
		if (_lhs.type().sizeOnStack() > 0)
			m_code <<
				(_declare ? "let ": "") <<
				_lhs.commaSeparatedList() <<
				" := ";
		m_code << m_context.utils().conversionFunction(_rhs.type(), _lhs.type()) <<
			"(" <<
			_rhs.commaSeparatedList() <<
			")\n";
	}
}

IRVariable IRGeneratorForStatements::zeroValue(Type const& _type, bool _splitFunctionTypes)
{
	IRVariable irVar{
		"zero_value_for_type_" + _type.identifier() + m_context.newYulVariable(),
		_type
	};
	define(irVar) << m_utils.zeroValueFunction(_type, _splitFunctionTypes) << "()\n";
	return irVar;
}

void IRGeneratorForStatements::appendSimpleUnaryOperation(UnaryOperation const& _operation, Expression const& _expr)
{
	string func;

	if (_operation.getOperator() == Token::Not)
		func = "iszero";
	else if (_operation.getOperator() == Token::BitNot)
		func = "not";
	else
		solAssert(false, "Invalid Token!");

	define(_operation) <<
		m_utils.cleanupFunction(type(_expr)) <<
		"(" <<
			func <<
			"(" <<
			IRVariable(_expr).commaSeparatedList() <<
			")" <<
		")\n";
}

string IRGeneratorForStatements::binaryOperation(
	langutil::Token _operator,
	Type const& _type,
	string const& _left,
	string const& _right
)
{
	if (IntegerType const* type = dynamic_cast<IntegerType const*>(&_type))
	{
		string fun;
		// TODO: Implement all operations for signed and unsigned types.
		switch (_operator)
		{
			case Token::Add:
				fun = m_utils.overflowCheckedIntAddFunction(*type);
				break;
			case Token::Sub:
				fun = m_utils.overflowCheckedIntSubFunction(*type);
				break;
			case Token::Mul:
				fun = m_utils.overflowCheckedIntMulFunction(*type);
				break;
			case Token::Div:
				fun = m_utils.overflowCheckedIntDivFunction(*type);
				break;
			case Token::Mod:
				fun = m_utils.checkedIntModFunction(*type);
				break;
			case Token::BitOr:
				fun = "or";
				break;
			case Token::BitXor:
				fun = "xor";
				break;
			case Token::BitAnd:
				fun = "and";
				break;
			default:
				break;
		}

		solUnimplementedAssert(!fun.empty(), "");
		return fun + "(" + _left + ", " + _right + ")\n";
	}
	else
		solUnimplementedAssert(false, "");

	return {};
}

void IRGeneratorForStatements::appendAndOrOperatorCode(BinaryOperation const& _binOp)
{
	langutil::Token const op = _binOp.getOperator();
	solAssert(op == Token::Or || op == Token::And, "");

	_binOp.leftExpression().accept(*this);

	IRVariable value(_binOp);
	define(value, _binOp.leftExpression());
	if (op == Token::Or)
		m_code << "if iszero(" << value.name() << ") {\n";
	else
		m_code << "if " << value.name() << " {\n";
	_binOp.rightExpression().accept(*this);
	assign(value, _binOp.rightExpression());
	m_code << "}\n";
}

void IRGeneratorForStatements::writeToLValue(IRLValue const& _lvalue, IRVariable const& _value)
{
	std::visit(
		util::GenericVisitor{
			[&](IRLValue::Storage const& _storage) {
				std::optional<unsigned> offset;

				if (std::holds_alternative<unsigned>(_storage.offset))
					offset = std::get<unsigned>(_storage.offset);

				m_code <<
					m_utils.updateStorageValueFunction(_lvalue.type, offset) <<
					"(" <<
					_storage.slot <<
					(
						std::holds_alternative<string>(_storage.offset) ?
						(", " + std::get<string>(_storage.offset)) :
						""
					) <<
					_value.commaSeparatedListPrefixed() <<
					")\n";
			},
			[&](IRLValue::Memory const& _memory) {
				if (_lvalue.type.isValueType())
				{
					IRVariable prepared(m_context.newYulVariable(), _lvalue.type);
					define(prepared, _value);

					if (_memory.byteArrayElement)
					{
						solAssert(_lvalue.type == *TypeProvider::byte(), "");
						m_code << "mstore8(" + _memory.address + ", byte(0, " + prepared.commaSeparatedList() + "))\n";
					}
					else
						m_code << m_utils.writeToMemoryFunction(_lvalue.type) <<
							"(" <<
							_memory.address <<
							", " <<
							prepared.commaSeparatedList() <<
							")\n";
				}
				else
				{
					solAssert(_lvalue.type.sizeOnStack() == 1, "");
					solAssert(dynamic_cast<ReferenceType const*>(&_lvalue.type), "");
					auto const* valueReferenceType = dynamic_cast<ReferenceType const*>(&_value.type());
					solAssert(valueReferenceType && valueReferenceType->dataStoredIn(DataLocation::Memory), "");
					m_code << "mstore(" + _memory.address + ", " + _value.part("mpos").name() + ")\n";
				}
			},
			[&](IRLValue::Stack const& _stack) { assign(_stack.variable, _value); },
			[&](IRLValue::Tuple const& _tuple) {
				auto components = std::move(_tuple.components);
				for (size_t i = 0; i < components.size(); i++)
				{
					size_t idx = components.size() - i - 1;
					if (components[idx])
						writeToLValue(*components[idx], _value.tupleComponent(idx));
				}
			}
		},
		_lvalue.kind
	);
}

IRVariable IRGeneratorForStatements::readFromLValue(IRLValue const& _lvalue)
{
	IRVariable result{m_context.newYulVariable(), _lvalue.type};
	std::visit(GenericVisitor{
		[&](IRLValue::Storage const& _storage) {
			if (!_lvalue.type.isValueType())
				define(result) << _storage.slot << "\n";
			else if (std::holds_alternative<string>(_storage.offset))
				define(result) <<
					m_utils.readFromStorageDynamic(_lvalue.type, false) <<
					"(" <<
					_storage.slot <<
					", " <<
					std::get<string>(_storage.offset) <<
					")\n";
			else
				define(result) <<
					m_utils.readFromStorage(_lvalue.type, std::get<unsigned>(_storage.offset), false) <<
					"(" <<
					_storage.slot <<
					")\n";
		},
		[&](IRLValue::Memory const& _memory) {
			if (_memory.byteArrayElement)
				define(result) <<
					m_utils.cleanupFunction(_lvalue.type) <<
					"(mload(" <<
					_memory.address <<
					"))\n";
			else if (_lvalue.type.isValueType())
				define(result) <<
					m_utils.readFromMemory(_lvalue.type) <<
					"(" <<
					_memory.address <<
					")\n";
			else
				define(result) << "mload(" << _memory.address << ")\n";
		},
		[&](IRLValue::Stack const& _stack) {
			define(result, _stack.variable);
		},
		[&](IRLValue::Tuple const&) {
			solAssert(false, "Attempted to read from tuple lvalue.");
		}
	}, _lvalue.kind);
	return result;
}

void IRGeneratorForStatements::setLValue(Expression const& _expression, IRLValue _lvalue)
{
	solAssert(!m_currentLValue, "");

	if (_expression.annotation().willBeWrittenTo)
	{
		m_currentLValue.emplace(std::move(_lvalue));
		solAssert(!_lvalue.type.dataStoredIn(DataLocation::CallData), "");
	}
	else
		// Only define the expression, if it will not be written to.
		define(_expression, readFromLValue(_lvalue));
}

void IRGeneratorForStatements::generateLoop(
	Statement const& _body,
	Expression const* _conditionExpression,
	Statement const*  _initExpression,
	ExpressionStatement const* _loopExpression,
	bool _isDoWhile
)
{
	string firstRun;

	if (_isDoWhile)
	{
		solAssert(_conditionExpression, "Expected condition for doWhile");
		firstRun = m_context.newYulVariable();
		m_code << "let " << firstRun << " := 1\n";
	}

	m_code << "for {\n";
	if (_initExpression)
		_initExpression->accept(*this);
	m_code << "} 1 {\n";
	if (_loopExpression)
		_loopExpression->accept(*this);
	m_code << "}\n";
	m_code << "{\n";

	if (_conditionExpression)
	{
		if (_isDoWhile)
			m_code << "if iszero(" << firstRun << ") {\n";

		_conditionExpression->accept(*this);
		m_code <<
			"if iszero(" <<
			expressionAsType(*_conditionExpression, *TypeProvider::boolean()) <<
			") { break }\n";

		if (_isDoWhile)
			m_code << "}\n" << firstRun << " := 0\n";
	}

	_body.accept(*this);

	m_code << "}\n";
}

Type const& IRGeneratorForStatements::type(Expression const& _expression)
{
	solAssert(_expression.annotation().type, "Type of expression not set.");
	return *_expression.annotation().type;
}

bool IRGeneratorForStatements::visit(TryStatement const& _tryStatement)
{
	Expression const& externalCall = _tryStatement.externalCall();
	externalCall.accept(*this);

	m_code << "switch iszero(" << m_context.trySuccessConditionVariable(externalCall) << ")\n";

	m_code << "case 0 { // success case\n";
	TryCatchClause const& successClause = *_tryStatement.clauses().front();
	if (successClause.parameters())
	{
		size_t i = 0;
		for (ASTPointer<VariableDeclaration> const& varDecl: successClause.parameters()->parameters())
		{
			solAssert(varDecl, "");
			define(m_context.addLocalVariable(*varDecl),
				successClause.parameters()->parameters().size() == 1 ?
				IRVariable(externalCall) :
				IRVariable(externalCall).tupleComponent(i++)
			);
		}
	}

	successClause.block().accept(*this);
	m_code << "}\n";

	m_code << "default { // failure case\n";
	handleCatch(_tryStatement);
	m_code << "}\n";

	return false;
}

void IRGeneratorForStatements::handleCatch(TryStatement const& _tryStatement)
{
	if (_tryStatement.structuredClause())
		handleCatchStructuredAndFallback(*_tryStatement.structuredClause(), _tryStatement.fallbackClause());
	else if (_tryStatement.fallbackClause())
		handleCatchFallback(*_tryStatement.fallbackClause());
	else
		rethrow();
}

void IRGeneratorForStatements::handleCatchStructuredAndFallback(
	TryCatchClause const& _structured,
	TryCatchClause const* _fallback
)
{
	solAssert(
		_structured.parameters() &&
		_structured.parameters()->parameters().size() == 1 &&
		_structured.parameters()->parameters().front() &&
		*_structured.parameters()->parameters().front()->annotation().type == *TypeProvider::stringMemory(),
		""
	);
	solAssert(m_context.evmVersion().supportsReturndata(), "");

	// Try to decode the error message.
	// If this fails, leaves 0 on the stack, otherwise the pointer to the data string.
	string const dataVariable = m_context.newYulVariable();

	m_code << "let " << dataVariable << " := " << m_utils.tryDecodeErrorMessageFunction() << "()\n";
	m_code << "switch iszero(" << dataVariable << ") \n";
	m_code << "case 0 { // decoding success\n";
	if (_structured.parameters())
	{
		solAssert(_structured.parameters()->parameters().size() == 1, "");
		IRVariable const& var = m_context.addLocalVariable(*_structured.parameters()->parameters().front());
		define(var) << dataVariable << "\n";
	}
	_structured.accept(*this);
	m_code << "}\n";
	m_code << "default { // decoding failure\n";
	if (_fallback)
		handleCatchFallback(*_fallback);
	else
		rethrow();
	m_code << "}\n";
}

void IRGeneratorForStatements::handleCatchFallback(TryCatchClause const& _fallback)
{
	if (_fallback.parameters())
	{
		solAssert(m_context.evmVersion().supportsReturndata(), "");
		solAssert(
			_fallback.parameters()->parameters().size() == 1 &&
			_fallback.parameters()->parameters().front() &&
			*_fallback.parameters()->parameters().front()->annotation().type == *TypeProvider::bytesMemory(),
			""
		);

		VariableDeclaration const& paramDecl = *_fallback.parameters()->parameters().front();
		define(m_context.addLocalVariable(paramDecl)) << m_utils.extractReturndataFunction() << "()\n";
	}
	_fallback.accept(*this);
}

void IRGeneratorForStatements::rethrow()
{
	if (m_context.evmVersion().supportsReturndata())
		m_code << R"(
			returndatacopy(0, 0, returndatasize())
			revert(0, returndatasize())
		)"s;
	else
		m_code << "revert(0, 0) // rethrow\n"s;
}

bool IRGeneratorForStatements::visit(TryCatchClause const& _clause)
{
	_clause.block().accept(*this);
	return false;
}
