/****************************************************************************
**
** Copyright (C) 2012 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the V4VM module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qv4isel_masm_p.h"
#include "qmljs_runtime.h"
#include "qv4object.h"
#include "qv4functionobject.h"
#include "qv4regexpobject.h"

#include <assembler/LinkBuffer.h>
#include <WTFStubs.h>

#include <sys/mman.h>
#include <iostream>
#include <cassert>

#ifndef NO_UDIS86
#  include <udis86.h>
#endif

using namespace QQmlJS;
using namespace QQmlJS::MASM;
using namespace QQmlJS::VM;

Assembler::Assembler(IR::Function* function)
    : _function(function)
{
}

void Assembler::registerBlock(IR::BasicBlock* block)
{
    _addrs[block] = label();
}

void Assembler::jumpToBlock(IR::BasicBlock* current, IR::BasicBlock *target)
{
    if (current->index + 1 != target->index)
        _patches[target].append(jump());
}

void Assembler::addPatch(IR::BasicBlock* targetBlock, Jump targetJump)
{
    _patches[targetBlock].append(targetJump);
}

Assembler::Pointer Assembler::loadTempAddress(RegisterID reg, IR::Temp *t)
{
    int32_t offset = 0;
    if (t->index < 0) {
        const int arg = -t->index - 1;
        loadPtr(Address(ContextRegister, offsetof(ExecutionContext, arguments)), reg);
        offset = arg * sizeof(Value);
    } else if (t->index < _function->locals.size()) {
        loadPtr(Address(ContextRegister, offsetof(ExecutionContext, locals)), reg);
        offset = t->index * sizeof(Value);
    } else {
        const int arg = _function->maxNumberOfArguments + t->index - _function->locals.size() + 1;
        // StackFrameRegister points to its old value on the stack, so even for the first temp we need to
        // subtract at least sizeof(Value).
        offset = - sizeof(Value) * (arg + 1);
        reg = StackFrameRegister;
    }
    return Pointer(reg, offset);
}

template <typename Result, typename Source>
void Assembler::copyValue(Result result, Source source)
{
#ifdef VALUE_FITS_IN_REGISTER
    // Use ReturnValueRegister as "scratch" register because loadArgument
    // and storeArgument are functions that may need a scratch register themselves.
    loadArgument(source, ReturnValueRegister);
    storeArgument(ReturnValueRegister, result);
#else
    loadDouble(source, FPGpr0);
    storeDouble(FPGpr0, result);
#endif
}

void Assembler::storeValue(VM::Value value, IR::Temp* destination)
{
    Address addr = loadTempAddress(ScratchRegister, destination);
    storeValue(value, addr);
}

void Assembler::enterStandardStackFrame(int locals)
{
#if CPU(ARM)
    push(JSC::ARMRegisters::lr);
#endif
    push(StackFrameRegister);
    move(StackPointerRegister, StackFrameRegister);

    // space for the locals and the ContextRegister
    int32_t frameSize = locals * sizeof(QQmlJS::VM::Value) + sizeof(void*);

#if CPU(X86) || CPU(X86_64)
    frameSize = (frameSize + 15) & ~15; // align on 16 byte boundaries for MMX
#endif
    subPtr(TrustedImm32(frameSize), StackPointerRegister);

#if CPU(X86) || CPU(ARM)
    for (int saveReg = CalleeSavedFirstRegister; saveReg <= CalleeSavedLastRegister; ++saveReg)
        push(static_cast<RegisterID>(saveReg));
#endif
    // save the ContextRegister
    storePtr(ContextRegister, StackPointerRegister);
}

void Assembler::leaveStandardStackFrame(int locals)
{
    // restore the ContextRegister
    loadPtr(StackPointerRegister, ContextRegister);

#if CPU(X86) || CPU(ARM)
    for (int saveReg = CalleeSavedLastRegister; saveReg >= CalleeSavedFirstRegister; --saveReg)
        pop(static_cast<RegisterID>(saveReg));
#endif
    // space for the locals and the ContextRegister
    int32_t frameSize = locals * sizeof(QQmlJS::VM::Value) + sizeof(void*);
#if CPU(X86) || CPU(X86_64)
    frameSize = (frameSize + 15) & ~15; // align on 16 byte boundaries for MMX
#endif
    addPtr(TrustedImm32(frameSize), StackPointerRegister);

    pop(StackFrameRegister);
#if CPU(ARM)
    pop(JSC::ARMRegisters::lr);
#endif
}



#define OP(op) \
    { isel_stringIfy(op), op, 0, 0 }

#define INLINE_OP(op, memOp, immOp) \
    { isel_stringIfy(op), op, memOp, immOp }

#define NULL_OP \
    { 0, 0, 0, 0 }

const Assembler::BinaryOperationInfo Assembler::binaryOperations[QQmlJS::IR::LastAluOp + 1] = {
    NULL_OP, // OpInvalid
    NULL_OP, // OpIfTrue
    NULL_OP, // OpNot
    NULL_OP, // OpUMinus
    NULL_OP, // OpUPlus
    NULL_OP, // OpCompl
    NULL_OP, // OpIncrement
    NULL_OP, // OpDecrement

    INLINE_OP(__qmljs_bit_and, &Assembler::inline_and32, &Assembler::inline_and32), // OpBitAnd
    INLINE_OP(__qmljs_bit_or, &Assembler::inline_or32, &Assembler::inline_or32), // OpBitOr
    INLINE_OP(__qmljs_bit_xor, &Assembler::inline_xor32, &Assembler::inline_xor32), // OpBitXor

    INLINE_OP(__qmljs_add, &Assembler::inline_add32, &Assembler::inline_add32), // OpAdd
    INLINE_OP(__qmljs_sub, &Assembler::inline_sub32, &Assembler::inline_sub32), // OpSub
    INLINE_OP(__qmljs_mul, &Assembler::inline_mul32, &Assembler::inline_mul32), // OpMul

    OP(__qmljs_div), // OpDiv
    OP(__qmljs_mod), // OpMod

    INLINE_OP(__qmljs_shl, &Assembler::inline_shl32, &Assembler::inline_shl32), // OpLShift
    INLINE_OP(__qmljs_shr, &Assembler::inline_shr32, &Assembler::inline_shr32), // OpRShift
    INLINE_OP(__qmljs_ushr, &Assembler::inline_ushr32, &Assembler::inline_ushr32), // OpURShift

    OP(__qmljs_gt), // OpGt
    OP(__qmljs_lt), // OpLt
    OP(__qmljs_ge), // OpGe
    OP(__qmljs_le), // OpLe
    OP(__qmljs_eq), // OpEqual
    OP(__qmljs_ne), // OpNotEqual
    OP(__qmljs_se), // OpStrictEqual
    OP(__qmljs_sne), // OpStrictNotEqual

    OP(__qmljs_instanceof), // OpInstanceof
    OP(__qmljs_in), // OpIn

    NULL_OP, // OpAnd
    NULL_OP // OpOr
};

void Assembler::generateBinOp(IR::AluOp operation, IR::Temp* target, IR::Expr* left, IR::Expr* right)
{
    const BinaryOperationInfo& info = binaryOperations[operation];
    if (!info.fallbackImplementation) {
        assert(!"unreachable");
        return;
    }

    Value leftConst = Value::undefinedValue();
    Value rightConst = Value::undefinedValue();

    bool canDoInline = info.inlineMemRegOp && info.inlineImmRegOp;

    if (canDoInline) {
        if (left->asConst()) {
            leftConst = convertToValue(left->asConst());
            canDoInline = canDoInline && leftConst.tryIntegerConversion();
        }
        if (right->asConst()) {
            rightConst = convertToValue(right->asConst());
            canDoInline = canDoInline && rightConst.tryIntegerConversion();
        }
    }

    Jump binOpFinished;

    if (canDoInline) {

        Jump leftTypeCheck;
        if (left->asTemp()) {
            Address typeAddress = loadTempAddress(ScratchRegister, left->asTemp());
            typeAddress.offset += offsetof(VM::Value, tag);
            leftTypeCheck = branch32(NotEqual, typeAddress, TrustedImm32(VM::Value::_Integer_Type));
        }

        Jump rightTypeCheck;
        if (right->asTemp()) {
            Address typeAddress = loadTempAddress(ScratchRegister, right->asTemp());
            typeAddress.offset += offsetof(VM::Value, tag);
            rightTypeCheck = branch32(NotEqual, typeAddress, TrustedImm32(VM::Value::_Integer_Type));
        }

        if (left->asTemp()) {
            Address leftValue = loadTempAddress(ScratchRegister, left->asTemp());
            leftValue.offset += offsetof(VM::Value, int_32);
            load32(leftValue, IntegerOpRegister);
        } else { // left->asConst()
            move(TrustedImm32(leftConst.integerValue()), IntegerOpRegister);
        }

        Jump overflowCheck;

        if (right->asTemp()) {
            Address rightValue = loadTempAddress(ScratchRegister, right->asTemp());
            rightValue.offset += offsetof(VM::Value, int_32);

            overflowCheck = (this->*info.inlineMemRegOp)(rightValue, IntegerOpRegister);
        } else { // right->asConst()
            overflowCheck = (this->*info.inlineImmRegOp)(TrustedImm32(rightConst.integerValue()), IntegerOpRegister);
        }

        Address resultAddr = loadTempAddress(ScratchRegister, target);
        Address resultValueAddr = resultAddr;
        resultValueAddr.offset += offsetof(VM::Value, int_32);
        store32(IntegerOpRegister, resultValueAddr);

        Address resultTypeAddr = resultAddr;
        resultTypeAddr.offset += offsetof(VM::Value, tag);
        store32(TrustedImm32(VM::Value::_Integer_Type), resultTypeAddr);

        binOpFinished = jump();

        if (leftTypeCheck.isSet())
            leftTypeCheck.link(this);
        if (rightTypeCheck.isSet())
            rightTypeCheck.link(this);
        if (overflowCheck.isSet())
            overflowCheck.link(this);
    }

    // Fallback
    generateFunctionCallImp(target, info.name, info.fallbackImplementation, left, right, ContextRegister);

    if (binOpFinished.isSet())
        binOpFinished.link(this);
}
#if OS(LINUX)
static void printDisassembledOutputWithCalls(const char* output, const QHash<void*, const char*>& functions)
{
    QByteArray processedOutput(output);
    for (QHash<void*, const char*>::ConstIterator it = functions.begin(), end = functions.end();
         it != end; ++it) {
        QByteArray ptrString = QByteArray::number(qlonglong(it.key()), 16);
        ptrString.prepend("0x");
        processedOutput = processedOutput.replace(ptrString, it.value());
    }
    fprintf(stderr, "%s\n", processedOutput.constData());
}
#endif

void Assembler::link(VM::Function *vmFunc)
{
    QHashIterator<IR::BasicBlock *, QVector<Jump> > it(_patches);
    while (it.hasNext()) {
        it.next();
        IR::BasicBlock *block = it.key();
        Label target = _addrs.value(block);
        assert(target.isSet());
        foreach (Jump jump, it.value())
            jump.linkTo(target, this);
    }

    JSC::JSGlobalData dummy;
    JSC::LinkBuffer linkBuffer(dummy, this, 0);
    QHash<void*, const char*> functions;
    foreach (CallToLink ctl, _callsToLink) {
        linkBuffer.link(ctl.call, ctl.externalFunction);
        functions[ctl.externalFunction.value()] = ctl.functionName;
    }

    static bool showCode = !qgetenv("SHOW_CODE").isNull();
    if (showCode) {
#if OS(LINUX)
        char* disasmOutput = 0;
        size_t disasmLength = 0;
        FILE* disasmStream = open_memstream(&disasmOutput, &disasmLength);
        WTF::setDataFile(disasmStream);
#endif

        QByteArray name = _function->name->toUtf8();
        vmFunc->codeRef = linkBuffer.finalizeCodeWithDisassembly("%s", name.data());

        WTF::setDataFile(stderr);
#if OS(LINUX)
        fclose(disasmStream);
#if CPU(X86) || CPU(X86_64)
        printDisassembledOutputWithCalls(disasmOutput, functions);
#endif
        free(disasmOutput);
#endif
    } else {
        vmFunc->codeRef = linkBuffer.finalizeCodeWithoutDisassembly();
    }

    vmFunc->code = (Value (*)(VM::ExecutionContext *, const uchar *)) vmFunc->codeRef.code().executableAddress();
}

InstructionSelection::InstructionSelection(VM::ExecutionEngine *engine, IR::Module *module)
    : EvalInstructionSelection(engine, module)
    , _block(0)
    , _function(0)
    , _vmFunction(0)
    , _asm(0)
{
}

InstructionSelection::~InstructionSelection()
{
    delete _asm;
}

void InstructionSelection::run(VM::Function *vmFunction, IR::Function *function)
{
    qSwap(_function, function);
    qSwap(_vmFunction, vmFunction);
    Assembler* oldAssembler = _asm;
    _asm = new Assembler(_function);

    int locals = (_function->tempCount - _function->locals.size() + _function->maxNumberOfArguments) + 1;
    locals = (locals + 1) & ~1;
    _asm->enterStandardStackFrame(locals);

    int contextPointer = 0;
#ifndef VALUE_FITS_IN_REGISTER
    // When the return VM value doesn't fit into a register, then
    // the caller provides a pointer for storage as first argument.
    // That shifts the index the context pointer argument by one.
    contextPointer++;
#endif
#if CPU(X86)
    _asm->loadPtr(addressForArgument(contextPointer), Assembler::ContextRegister);
#elif CPU(X86_64) || CPU(ARM)
    _asm->move(_asm->registerForArgument(contextPointer), Assembler::ContextRegister);
#else
    assert(!"TODO");
#endif

    foreach (IR::BasicBlock *block, _function->basicBlocks) {
        _block = block;
        _asm->registerBlock(_block);
        foreach (IR::Stmt *s, block->statements) {
            s->accept(this);
        }
    }

    _asm->leaveStandardStackFrame(locals);
#ifndef VALUE_FITS_IN_REGISTER
    // Emulate ret(n) instruction
    // Pop off return address into scratch register ...
    _asm->pop(Assembler::ScratchRegister);
    // ... and overwrite the invisible argument with
    // the return address.
    _asm->poke(Assembler::ScratchRegister);
#endif
    _asm->ret();

    _asm->link(_vmFunction);

    qSwap(_vmFunction, vmFunction);
    qSwap(_function, function);
    delete _asm;
    _asm = oldAssembler;
}

void InstructionSelection::callBuiltinInvalid(IR::Name *func, IR::ExprList *args, IR::Temp *result)
{
    callRuntimeMethod(result, __qmljs_call_activation_property, func, args);
}

void InstructionSelection::callBuiltinTypeofMember(IR::Temp *base, const QString &name, IR::Temp *result)
{
    generateFunctionCall(result, __qmljs_builtin_typeof_member, base, identifier(name), Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinTypeofSubscript(IR::Temp *base, IR::Temp *index, IR::Temp *result)
{
    generateFunctionCall(result, __qmljs_builtin_typeof_element, base, index, Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinTypeofName(const QString &name, IR::Temp *result)
{
    generateFunctionCall(result, __qmljs_builtin_typeof_name, identifier(name), Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinTypeofValue(IR::Temp *value, IR::Temp *result)
{
    generateFunctionCall(result, __qmljs_builtin_typeof, value, Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinDeleteMember(IR::Temp *base, const QString &name, IR::Temp *result)
{
    generateFunctionCall(result, __qmljs_delete_member, Assembler::ContextRegister, base, identifier(name));
}

void InstructionSelection::callBuiltinDeleteSubscript(IR::Temp *base, IR::Temp *index, IR::Temp *result)
{
    generateFunctionCall(result, __qmljs_delete_subscript, Assembler::ContextRegister, base, index);
}

void InstructionSelection::callBuiltinDeleteName(const QString &name, IR::Temp *result)
{
    generateFunctionCall(result, __qmljs_delete_name, Assembler::ContextRegister, identifier(name));
}

void InstructionSelection::callBuiltinDeleteValue(IR::Temp *result)
{
    _asm->storeValue(Value::fromBoolean(false), result);
}

void InstructionSelection::callBuiltinThrow(IR::Temp *arg)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_throw, arg, Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinCreateExceptionHandler(IR::Temp *result)
{
    generateFunctionCall(Assembler::ReturnValueRegister, __qmljs_create_exception_handler, Assembler::ContextRegister);
    generateFunctionCall(Assembler::ReturnValueRegister, setjmp, Assembler::ReturnValueRegister);
    Address addr = _asm->loadTempAddress(Assembler::ScratchRegister, result);
    _asm->store32(Assembler::ReturnValueRegister, addr);
    addr.offset += 4;
    _asm->store32(Assembler::TrustedImm32(Value::Boolean_Type), addr);
}

void InstructionSelection::callBuiltinDeleteExceptionHandler()
{
    generateFunctionCall(Assembler::Void, __qmljs_delete_exception_handler, Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinGetException(IR::Temp *result)
{
    generateFunctionCall(result, __qmljs_get_exception, Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinForeachIteratorObject(IR::Temp *arg, IR::Temp *result)
{
    generateFunctionCall(result, __qmljs_foreach_iterator_object, arg, Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinForeachNextPropertyname(IR::Temp *arg, IR::Temp *result)
{
    generateFunctionCall(result, __qmljs_foreach_next_property_name, arg);
}

void InstructionSelection::callBuiltinPushWith(IR::Temp *arg)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_push_with, arg, Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinPopWith()
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_pop_with, Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinDeclareVar(bool deletable, const QString &name)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_declare_var, Assembler::ContextRegister,
                         Assembler::TrustedImm32(deletable), identifier(name));
}

void InstructionSelection::callBuiltinDefineGetterSetter(IR::Temp *object, const QString &name, IR::Temp *getter, IR::Temp *setter)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_define_getter_setter,
                         object, identifier(name), getter, setter, Assembler::ContextRegister);
}

void InstructionSelection::callBuiltinDefineProperty(IR::Temp *object, const QString &name, IR::Temp *value)
{
    generateFunctionCall(Assembler::Void, __qmljs_builtin_define_property,
                         object, identifier(name), value, Assembler::ContextRegister);
}

void InstructionSelection::callValue(IR::Temp *value, IR::ExprList *args, IR::Temp *result)
{
    int argc = prepareVariableArguments(args);
    IR::Temp* thisObject = 0;
    generateFunctionCall(result, __qmljs_call_value, Assembler::ContextRegister, thisObject, value, baseAddressForCallArguments(), Assembler::TrustedImm32(argc));
}

void InstructionSelection::loadThisObject(IR::Temp *temp)
{
    generateFunctionCall(temp, __qmljs_get_thisObject, Assembler::ContextRegister);
}

void InstructionSelection::loadConst(IR::Const *sourceConst, IR::Temp *targetTemp)
{
    _asm->storeValue(convertToValue(sourceConst), targetTemp);
}

void InstructionSelection::loadString(const QString &str, IR::Temp *targetTemp)
{
    Value v = Value::fromString(engine()->newString(str));
    _asm->storeValue(v, targetTemp);
}

void InstructionSelection::loadRegexp(IR::RegExp *sourceRegexp, IR::Temp *targetTemp)
{
    Value v = Value::fromObject(engine()->newRegExpObject(*sourceRegexp->value,
                                                          sourceRegexp->flags));
    _vmFunction->generatedValues.append(v);
    _asm->storeValue(v, targetTemp);
}

void InstructionSelection::getActivationProperty(const QString &name, IR::Temp *temp)
{
    String *propertyName = identifier(name);
    generateFunctionCall(temp, __qmljs_get_activation_property, Assembler::ContextRegister, propertyName);
}

void InstructionSelection::setActivationProperty(IR::Expr *source, const QString &targetName)
{
    String *propertyName = identifier(targetName);
    generateFunctionCall(Assembler::Void, __qmljs_set_activation_property, Assembler::ContextRegister, propertyName, source);
}

void InstructionSelection::initClosure(IR::Closure *closure, IR::Temp *target)
{
    VM::Function *vmFunc = vmFunction(closure->value);
    assert(vmFunc);
    generateFunctionCall(target, __qmljs_init_closure, Assembler::TrustedImmPtr(vmFunc), Assembler::ContextRegister);
}

void InstructionSelection::getProperty(IR::Temp *base, const QString &name, IR::Temp *target)
{
    generateFunctionCall(target, __qmljs_get_property, Assembler::ContextRegister, base, identifier(name));
}

void InstructionSelection::setProperty(IR::Expr *source, IR::Temp *targetBase, const QString &targetName)
{
    generateFunctionCall(Assembler::Void, __qmljs_set_property, Assembler::ContextRegister, targetBase, identifier(targetName), source);
}

void InstructionSelection::getElement(IR::Temp *base, IR::Temp *index, IR::Temp *target)
{
    generateFunctionCall(target, __qmljs_get_element, Assembler::ContextRegister, base, index);
}

void InstructionSelection::setElement(IR::Expr *source, IR::Temp *targetBase, IR::Temp *targetIndex)
{
    generateFunctionCall(Assembler::Void, __qmljs_set_element, Assembler::ContextRegister, targetBase, targetIndex, source);
}

void InstructionSelection::copyValue(IR::Temp *sourceTemp, IR::Temp *targetTemp)
{
    _asm->copyValue(targetTemp, sourceTemp);
}

#define setOp(op, opName, operation) \
    do { op = operation; opName = isel_stringIfy(operation); } while (0)

void InstructionSelection::unop(IR::AluOp oper, IR::Temp *sourceTemp, IR::Temp *targetTemp)
{
    Value (*op)(const Value value, ExecutionContext *ctx) = 0;
    const char *opName = 0;
    switch (oper) {
    case IR::OpIfTrue: assert(!"unreachable"); break;
    case IR::OpNot: setOp(op, opName, __qmljs_not); break;
    case IR::OpUMinus: setOp(op, opName, __qmljs_uminus); break;
    case IR::OpUPlus: setOp(op, opName, __qmljs_uplus); break;
    case IR::OpCompl: setOp(op, opName, __qmljs_compl); break;
    case IR::OpIncrement: setOp(op, opName, __qmljs_increment); break;
    case IR::OpDecrement: setOp(op, opName, __qmljs_decrement); break;
    default: assert(!"unreachable"); break;
    } // switch

    if (op)
        _asm->generateFunctionCallImp(targetTemp, opName, op, sourceTemp,
                                      Assembler::ContextRegister);
}

void InstructionSelection::binop(IR::AluOp oper, IR::Expr *leftSource, IR::Expr *rightSource, IR::Temp *target)
{
    _asm->generateBinOp(oper, target, leftSource, rightSource);
}

void InstructionSelection::inplaceNameOp(IR::AluOp oper, IR::Expr *sourceExpr, const QString &targetName)
{
    void (*op)(const Value value, String *name, ExecutionContext *ctx) = 0;
    const char *opName = 0;
    switch (oper) {
    case IR::OpBitAnd: setOp(op, opName, __qmljs_inplace_bit_and_name); break;
    case IR::OpBitOr: setOp(op, opName, __qmljs_inplace_bit_or_name); break;
    case IR::OpBitXor: setOp(op, opName, __qmljs_inplace_bit_xor_name); break;
    case IR::OpAdd: setOp(op, opName, __qmljs_inplace_add_name); break;
    case IR::OpSub: setOp(op, opName, __qmljs_inplace_sub_name); break;
    case IR::OpMul: setOp(op, opName, __qmljs_inplace_mul_name); break;
    case IR::OpDiv: setOp(op, opName, __qmljs_inplace_div_name); break;
    case IR::OpMod: setOp(op, opName, __qmljs_inplace_mod_name); break;
    case IR::OpLShift: setOp(op, opName, __qmljs_inplace_shl_name); break;
    case IR::OpRShift: setOp(op, opName, __qmljs_inplace_shr_name); break;
    case IR::OpURShift: setOp(op, opName, __qmljs_inplace_ushr_name); break;
    default:
        Q_UNREACHABLE();
        break;
    }
    if (op) {
        _asm->generateFunctionCallImp(Assembler::Void, opName, op, sourceExpr, identifier(targetName), Assembler::ContextRegister);
    }
}

void InstructionSelection::inplaceElementOp(IR::AluOp oper, IR::Expr *sourceExpr, IR::Temp *targetBaseTemp, IR::Temp *targetIndexTemp)
{
    void (*op)(Value base, Value index, Value value, ExecutionContext *ctx) = 0;
    const char *opName = 0;
    switch (oper) {
    case IR::OpBitAnd: setOp(op, opName, __qmljs_inplace_bit_and_element); break;
    case IR::OpBitOr: setOp(op, opName, __qmljs_inplace_bit_or_element); break;
    case IR::OpBitXor: setOp(op, opName, __qmljs_inplace_bit_xor_element); break;
    case IR::OpAdd: setOp(op, opName, __qmljs_inplace_add_element); break;
    case IR::OpSub: setOp(op, opName, __qmljs_inplace_sub_element); break;
    case IR::OpMul: setOp(op, opName, __qmljs_inplace_mul_element); break;
    case IR::OpDiv: setOp(op, opName, __qmljs_inplace_div_element); break;
    case IR::OpMod: setOp(op, opName, __qmljs_inplace_mod_element); break;
    case IR::OpLShift: setOp(op, opName, __qmljs_inplace_shl_element); break;
    case IR::OpRShift: setOp(op, opName, __qmljs_inplace_shr_element); break;
    case IR::OpURShift: setOp(op, opName, __qmljs_inplace_ushr_element); break;
    default:
        Q_UNREACHABLE();
        break;
    }

    if (op) {
        _asm->generateFunctionCallImp(Assembler::Void, opName, op, targetBaseTemp, targetIndexTemp, sourceExpr, Assembler::ContextRegister);
    }
}

void InstructionSelection::inplaceMemberOp(IR::AluOp oper, IR::Expr *source, IR::Temp *targetBase, const QString &targetName)
{
    void (*op)(Value value, Value base, String *name, ExecutionContext *ctx) = 0;
    const char *opName = 0;
    switch (oper) {
    case IR::OpBitAnd: setOp(op, opName, __qmljs_inplace_bit_and_member); break;
    case IR::OpBitOr: setOp(op, opName, __qmljs_inplace_bit_or_member); break;
    case IR::OpBitXor: setOp(op, opName, __qmljs_inplace_bit_xor_member); break;
    case IR::OpAdd: setOp(op, opName, __qmljs_inplace_add_member); break;
    case IR::OpSub: setOp(op, opName, __qmljs_inplace_sub_member); break;
    case IR::OpMul: setOp(op, opName, __qmljs_inplace_mul_member); break;
    case IR::OpDiv: setOp(op, opName, __qmljs_inplace_div_member); break;
    case IR::OpMod: setOp(op, opName, __qmljs_inplace_mod_member); break;
    case IR::OpLShift: setOp(op, opName, __qmljs_inplace_shl_member); break;
    case IR::OpRShift: setOp(op, opName, __qmljs_inplace_shr_member); break;
    case IR::OpURShift: setOp(op, opName, __qmljs_inplace_ushr_member); break;
    default:
        Q_UNREACHABLE();
        break;
    }

    if (op) {
        String* member = identifier(targetName);
        _asm->generateFunctionCallImp(Assembler::Void, opName, op, source, targetBase, member, Assembler::ContextRegister);
    }
}

void InstructionSelection::callProperty(IR::Temp *base, const QString &name,
                                        IR::ExprList *args, IR::Temp *result)
{
    assert(base != 0);

    int argc = prepareVariableArguments(args);
    generateFunctionCall(result, __qmljs_call_property,
                         Assembler::ContextRegister, base, identifier(name),
                         baseAddressForCallArguments(),
                         Assembler::TrustedImm32(argc));
}

String *InstructionSelection::identifier(const QString &s)
{
    return engine()->identifier(s);
}

void InstructionSelection::constructActivationProperty(IR::Name *func, IR::ExprList *args, IR::Temp *result)
{
    assert(func != 0);

    callRuntimeMethod(result, __qmljs_construct_activation_property, func, args);
}

void InstructionSelection::constructProperty(IR::Temp *base, const QString &name, IR::ExprList *args, IR::Temp *result)
{
    int argc = prepareVariableArguments(args);
    generateFunctionCall(result, __qmljs_construct_property, Assembler::ContextRegister, base, identifier(name), baseAddressForCallArguments(), Assembler::TrustedImm32(argc));
}

void InstructionSelection::constructValue(IR::Temp *value, IR::ExprList *args, IR::Temp *result)
{
    assert(value != 0);

    int argc = prepareVariableArguments(args);
    generateFunctionCall(result, __qmljs_construct_value, Assembler::ContextRegister, value, baseAddressForCallArguments(), Assembler::TrustedImm32(argc));
}

void InstructionSelection::visitJump(IR::Jump *s)
{
    _asm->jumpToBlock(_block, s->target);
}

void InstructionSelection::visitCJump(IR::CJump *s)
{
    if (IR::Temp *t = s->cond->asTemp()) {
        Address temp = _asm->loadTempAddress(Assembler::ScratchRegister, t);
        Address tag = temp;
        tag.offset += offsetof(VM::Value, tag);
        Assembler::Jump booleanConversion = _asm->branch32(Assembler::NotEqual, tag, Assembler::TrustedImm32(VM::Value::Boolean_Type));

        Address data = temp;
        data.offset += offsetof(VM::Value, int_32);
        _asm->load32(data, Assembler::ReturnValueRegister);
        Assembler::Jump testBoolean = _asm->jump();

        booleanConversion.link(_asm);
        {
            generateFunctionCall(Assembler::ReturnValueRegister, __qmljs_to_boolean, t, Assembler::ContextRegister);
        }

        testBoolean.link(_asm);
        Assembler::Jump target = _asm->branch32(Assembler::NotEqual, Assembler::ReturnValueRegister, Assembler::TrustedImm32(0));
        _asm->addPatch(s->iftrue, target);

        _asm->jumpToBlock(_block, s->iffalse);
        return;
    } else if (IR::Binop *b = s->cond->asBinop()) {
        if ((b->left->asTemp() || b->left->asConst()) &&
            (b->right->asTemp() || b->right->asConst())) {
            Bool (*op)(const Value, const Value, ExecutionContext *ctx) = 0;
            const char *opName = 0;
            switch (b->op) {
            default: Q_UNREACHABLE(); assert(!"todo"); break;
            case IR::OpGt: setOp(op, opName, __qmljs_cmp_gt); break;
            case IR::OpLt: setOp(op, opName, __qmljs_cmp_lt); break;
            case IR::OpGe: setOp(op, opName, __qmljs_cmp_ge); break;
            case IR::OpLe: setOp(op, opName, __qmljs_cmp_le); break;
            case IR::OpEqual: setOp(op, opName, __qmljs_cmp_eq); break;
            case IR::OpNotEqual: setOp(op, opName, __qmljs_cmp_ne); break;
            case IR::OpStrictEqual: setOp(op, opName, __qmljs_cmp_se); break;
            case IR::OpStrictNotEqual: setOp(op, opName, __qmljs_cmp_sne); break;
            case IR::OpInstanceof: setOp(op, opName, __qmljs_cmp_instanceof); break;
            case IR::OpIn: setOp(op, opName, __qmljs_cmp_in); break;
            } // switch

            _asm->generateFunctionCallImp(Assembler::ReturnValueRegister, opName, op, b->left, b->right, Assembler::ContextRegister);

            Assembler::Jump target = _asm->branch32(Assembler::NotEqual, Assembler::ReturnValueRegister, Assembler::TrustedImm32(0));
            _asm->addPatch(s->iftrue, target);

            _asm->jumpToBlock(_block, s->iffalse);
            return;
        } else {
            assert(!"wip");
        }
        Q_UNIMPLEMENTED();
    }
    Q_UNIMPLEMENTED();
    assert(!"TODO");
}

void InstructionSelection::visitRet(IR::Ret *s)
{
    if (IR::Temp *t = s->expr->asTemp()) {
#ifdef VALUE_FITS_IN_REGISTER
        _asm->copyValue(Assembler::ReturnValueRegister, t);
#else
        _asm->loadPtr(addressForArgument(0), Assembler::ReturnValueRegister);
        _asm->copyValue(Address(Assembler::ReturnValueRegister, 0), t);
#endif
        return;
    }
    Q_UNIMPLEMENTED();
    Q_UNUSED(s);
}

int InstructionSelection::prepareVariableArguments(IR::ExprList* args)
{
    int argc = 0;
    for (IR::ExprList *it = args; it; it = it->next) {
        ++argc;
    }

    int i = 0;
    for (IR::ExprList *it = args; it; it = it->next, ++i) {
        IR::Temp *arg = it->expr->asTemp();
        assert(arg != 0);
        _asm->copyValue(argumentAddressForCall(i), arg);
    }

    return argc;
}

void InstructionSelection::callRuntimeMethodImp(IR::Temp *result, const char* name, ActivationMethod method, IR::Expr *base, IR::ExprList *args)
{
    IR::Name *baseName = base->asName();
    assert(baseName != 0);

    int argc = prepareVariableArguments(args);
    _asm->generateFunctionCallImp(result, name, method, Assembler::ContextRegister, identifier(*baseName->id), baseAddressForCallArguments(), Assembler::TrustedImm32(argc));
}

void InstructionSelection::callRuntimeMethodImp(IR::Temp *result, const char* name, BuiltinMethod method, IR::ExprList *args)
{
    int argc = prepareVariableArguments(args);
    _asm->generateFunctionCallImp(result, name, method, Assembler::ContextRegister, baseAddressForCallArguments(), Assembler::TrustedImm32(argc));
}


