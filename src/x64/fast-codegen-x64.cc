// Copyright 2009 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "codegen-inl.h"
#include "debug.h"
#include "fast-codegen.h"
#include "parser.h"

namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm_)

// Generate code for a JS function.  On entry to the function the receiver
// and arguments have been pushed on the stack left to right, with the
// return address on top of them.  The actual argument count matches the
// formal parameter count expected by the function.
//
// The live registers are:
//   o rdi: the JS function object being called (ie, ourselves)
//   o rsi: our context
//   o rbp: our caller's frame pointer
//   o rsp: stack pointer (pointing to return address)
//
// The function builds a JS frame.  Please see JavaScriptFrameConstants in
// frames-x64.h for its layout.
void FastCodeGenerator::Generate(FunctionLiteral* fun) {
  function_ = fun;
  SetFunctionPosition(fun);

  __ push(rbp);  // Caller's frame pointer.
  __ movq(rbp, rsp);
  __ push(rsi);  // Callee's context.
  __ push(rdi);  // Callee's JS Function.

  { Comment cmnt(masm_, "[ Allocate locals");
    int locals_count = fun->scope()->num_stack_slots();
    for (int i = 0; i < locals_count; i++) {
      __ PushRoot(Heap::kUndefinedValueRootIndex);
    }
  }

  { Comment cmnt(masm_, "[ Stack check");
    Label ok;
    __ CompareRoot(rsp, Heap::kStackLimitRootIndex);
    __ j(above_equal, &ok);
    StackCheckStub stub;
    __ CallStub(&stub);
    __ bind(&ok);
  }

  { Comment cmnt(masm_, "[ Declarations");
    VisitDeclarations(fun->scope()->declarations());
  }

  if (FLAG_trace) {
    __ CallRuntime(Runtime::kTraceEnter, 0);
  }

  { Comment cmnt(masm_, "[ Body");
    VisitStatements(fun->body());
  }

  { Comment cmnt(masm_, "[ return <undefined>;");
    // Emit a 'return undefined' in case control fell off the end of the
    // body.
    __ LoadRoot(rax, Heap::kUndefinedValueRootIndex);
  }
  { Comment cmnt(masm_, "Return sequence");
    SetReturnPosition(fun);

    if (return_label_.is_bound()) {
      __ jmp(&return_label_);
    } else {
      __ bind(&return_label_);

      if (FLAG_trace) {
        __ push(rax);
        __ CallRuntime(Runtime::kTraceExit, 1);
      }
      __ RecordJSReturn();

      // Do not use the leave instruction here because it is too short to
      // patch with the code required by the debugger.
      __ movq(rsp, rbp);
      __ pop(rbp);
      __ ret((fun->scope()->num_parameters() + 1) * kPointerSize);
#ifdef ENABLE_DEBUGGER_SUPPORT
      // Add padding that will be overwritten by a debugger breakpoint.  We
      // have just generated "movq rsp, rbp; pop rbp; ret k" with length 7
      // (3 + 1 + 3).
      const int kPadding = Debug::kX64JSReturnSequenceLength - 7;
      for (int i = 0; i < kPadding; ++i) {
        masm_->int3();
      }
#endif
    }
  }
}



void FastCodeGenerator::Move(Expression::Context context, Register source) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();
    case Expression::kEffect:
      break;
    case Expression::kValue:
      __ push(source);
      break;
    case Expression::kTest:
      TestAndBranch(source, true_label_, false_label_);
      break;
    case Expression::kValueTest: {
      Label discard;
      __ push(source);
      TestAndBranch(source, true_label_, &discard);
      __ bind(&discard);
      __ addq(rsp, Immediate(kPointerSize));
      __ jmp(false_label_);
      break;
    }
    case Expression::kTestValue: {
      Label discard;
      __ push(source);
      TestAndBranch(source, &discard, false_label_);
      __ bind(&discard);
      __ addq(rsp, Immediate(kPointerSize));
      __ jmp(true_label_);
      break;
    }
  }
}


void FastCodeGenerator::Move(Expression::Context context, Slot* source) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();
    case Expression::kEffect:
      break;
    case Expression::kValue:
      __ push(Operand(rbp, SlotOffset(source)));
      break;
    case Expression::kTest:  // Fall through.
    case Expression::kValueTest:  // Fall through.
    case Expression::kTestValue:
      __ movq(rax, Operand(rbp, SlotOffset(source)));
      Move(context, rax);
      break;
  }
}


void FastCodeGenerator::Move(Expression::Context context, Literal* expr) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();
    case Expression::kEffect:
      break;
    case Expression::kValue:
      __ Push(expr->handle());
      break;
    case Expression::kTest:  // Fall through.
    case Expression::kValueTest:  // Fall through.
    case Expression::kTestValue:
      __ Move(rax, expr->handle());
      Move(context, rax);
      break;
  }
}


void FastCodeGenerator::DropAndMove(Expression::Context context,
                                    Register source) {
  switch (context) {
    case Expression::kUninitialized:
      UNREACHABLE();
    case Expression::kEffect:
      __ addq(rsp, Immediate(kPointerSize));
      break;
    case Expression::kValue:
      __ movq(Operand(rsp, 0), source);
      break;
    case Expression::kTest:
      ASSERT(!source.is(rsp));
      __ addq(rsp, Immediate(kPointerSize));
      TestAndBranch(source, true_label_, false_label_);
      break;
    case Expression::kValueTest: {
      Label discard;
      __ movq(Operand(rsp, 0), source);
      TestAndBranch(source, true_label_, &discard);
      __ bind(&discard);
      __ addq(rsp, Immediate(kPointerSize));
      __ jmp(false_label_);
      break;
    }
    case Expression::kTestValue: {
      Label discard;
      __ movq(Operand(rsp, 0), source);
      TestAndBranch(source, &discard, false_label_);
      __ bind(&discard);
      __ addq(rsp, Immediate(kPointerSize));
      __ jmp(true_label_);
      break;
    }
  }
}


void FastCodeGenerator::TestAndBranch(Register source,
                                      Label* true_label,
                                      Label* false_label) {
  ASSERT_NE(NULL, true_label);
  ASSERT_NE(NULL, false_label);
  // Use the shared ToBoolean stub to compile the value in the register into
  // control flow to the code generator's true and false labels.  Perform
  // the fast checks assumed by the stub.

  // The undefined value is false.
  __ CompareRoot(source, Heap::kUndefinedValueRootIndex);
  __ j(equal, false_label);
  __ CompareRoot(source, Heap::kTrueValueRootIndex);  // True is true.
  __ j(equal, true_label);
  __ CompareRoot(source, Heap::kFalseValueRootIndex);  // False is false.
  __ j(equal, false_label);
  ASSERT_EQ(0, kSmiTag);
  __ SmiCompare(source, Smi::FromInt(0));  // The smi zero is false.
  __ j(equal, false_label);
  Condition is_smi = masm_->CheckSmi(source);  // All other smis are true.
  __ j(is_smi, true_label);

  // Call the stub for all other cases.
  __ push(source);
  ToBooleanStub stub;
  __ CallStub(&stub);
  __ testq(rax, rax);  // The stub returns nonzero for true.
  __ j(not_zero, true_label);
  __ jmp(false_label);
}


void FastCodeGenerator::DeclareGlobals(Handle<FixedArray> pairs) {
  // Call the runtime to declare the globals.
  __ push(rsi);  // The context is the first argument.
  __ Push(pairs);
  __ Push(Smi::FromInt(is_eval_ ? 1 : 0));
  __ CallRuntime(Runtime::kDeclareGlobals, 3);
  // Return value is ignored.
}


void FastCodeGenerator::VisitReturnStatement(ReturnStatement* stmt) {
  Comment cmnt(masm_, "[ ReturnStatement");
  SetStatementPosition(stmt);
  Expression* expr = stmt->expression();
  if (expr->AsLiteral() != NULL) {
    __ Move(rax, expr->AsLiteral()->handle());
  } else {
    Visit(expr);
    ASSERT_EQ(Expression::kValue, expr->context());
    __ pop(rax);
  }

  if (return_label_.is_bound()) {
    __ jmp(&return_label_);
  } else {
    __ bind(&return_label_);

    if (FLAG_trace) {
      __ push(rax);
      __ CallRuntime(Runtime::kTraceExit, 1);
    }

    __ RecordJSReturn();
    // Do not use the leave instruction here because it is too short to
    // patch with the code required by the debugger.
    __ movq(rsp, rbp);
    __ pop(rbp);
    __ ret((function_->scope()->num_parameters() + 1) * kPointerSize);
#ifdef ENABLE_DEBUGGER_SUPPORT
    // Add padding that will be overwritten by a debugger breakpoint.  We
    // have just generated "movq rsp, rbp; pop rbp; ret k" with length 7
    // (3 + 1 + 3).
    const int kPadding = Debug::kX64JSReturnSequenceLength - 7;
    for (int i = 0; i < kPadding; ++i) {
      masm_->int3();
    }
#endif
  }
}


void FastCodeGenerator::VisitFunctionLiteral(FunctionLiteral* expr) {
  Comment cmnt(masm_, "[ FunctionLiteral");

  // Build the function boilerplate and instantiate it.
  Handle<JSFunction> boilerplate = BuildBoilerplate(expr);
  if (HasStackOverflow()) return;

  ASSERT(boilerplate->IsBoilerplate());

  // Create a new closure.
  __ push(rsi);
  __ Push(boilerplate);
  __ CallRuntime(Runtime::kNewClosure, 2);
  Move(expr->context(), rax);
}


void FastCodeGenerator::VisitVariableProxy(VariableProxy* expr) {
  Comment cmnt(masm_, "[ VariableProxy");
  Expression* rewrite = expr->var()->rewrite();
  if (rewrite == NULL) {
    Comment cmnt(masm_, "Global variable");
    // Use inline caching. Variable name is passed in rcx and the global
    // object on the stack.
    __ push(CodeGenerator::GlobalObject());
    __ Move(rcx, expr->name());
    Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET_CONTEXT);

    // A test rax instruction following the call is used by the IC to
    // indicate that the inobject property case was inlined.  Ensure there
    // is no test rax instruction here.
    DropAndMove(expr->context(), rax);
  } else {
    Comment cmnt(masm_, "Stack slot");
    Move(expr->context(), rewrite->AsSlot());
  }
}


void FastCodeGenerator::VisitRegExpLiteral(RegExpLiteral* expr) {
  Comment cmnt(masm_, "[ RegExp Literal");
  Label done;
  // Registers will be used as follows:
  // rdi = JS function.
  // rbx = literals array.
  // rax = regexp literal.
  __ movq(rdi, Operand(rbp, JavaScriptFrameConstants::kFunctionOffset));
  __ movq(rbx, FieldOperand(rdi, JSFunction::kLiteralsOffset));
  int literal_offset =
    FixedArray::kHeaderSize + expr->literal_index() * kPointerSize;
  __ movq(rax, FieldOperand(rbx, literal_offset));
  __ CompareRoot(rax, Heap::kUndefinedValueRootIndex);
  __ j(not_equal, &done);
  // Create regexp literal using runtime function
  // Result will be in rax.
  __ push(rbx);
  __ Push(Smi::FromInt(expr->literal_index()));
  __ Push(expr->pattern());
  __ Push(expr->flags());
  __ CallRuntime(Runtime::kMaterializeRegExpLiteral, 4);
  // Label done:
  __ bind(&done);
  Move(expr->context(), rax);
}


void FastCodeGenerator::VisitObjectLiteral(ObjectLiteral* expr) {
  Comment cmnt(masm_, "[ ObjectLiteral");
  Label boilerplate_exists;

  __ movq(rdi, Operand(rbp, JavaScriptFrameConstants::kFunctionOffset));
  __ movq(rbx, FieldOperand(rdi, JSFunction::kLiteralsOffset));
  int literal_offset =
    FixedArray::kHeaderSize + expr->literal_index() * kPointerSize;
  __ movq(rax, FieldOperand(rbx, literal_offset));
  __ CompareRoot(rax, Heap::kUndefinedValueRootIndex);
  __ j(not_equal, &boilerplate_exists);
  // Create boilerplate if it does not exist.
  // Literal array (0).
  __ push(rbx);
  // Literal index (1).
  __ Push(Smi::FromInt(expr->literal_index()));
  // Constant properties (2).
  __ Push(expr->constant_properties());
  __ CallRuntime(Runtime::kCreateObjectLiteralBoilerplate, 3);
  __ bind(&boilerplate_exists);
  // rax contains boilerplate.
  // Clone boilerplate.
  __ push(rax);
  if (expr->depth() == 1) {
    __ CallRuntime(Runtime::kCloneShallowLiteralBoilerplate, 1);
  } else {
    __ CallRuntime(Runtime::kCloneLiteralBoilerplate, 1);
  }

  // If result_saved == true: the result is saved on top of the stack.
  // If result_saved == false: the result is not on the stack, just in rax.
  bool result_saved = false;

  for (int i = 0; i < expr->properties()->length(); i++) {
    ObjectLiteral::Property* property = expr->properties()->at(i);
    if (property->IsCompileTimeValue()) continue;

    Literal* key = property->key();
    Expression* value = property->value();
    if (!result_saved) {
      __ push(rax);  // Save result on the stack
      result_saved = true;
    }
    switch (property->kind()) {
      case ObjectLiteral::Property::MATERIALIZED_LITERAL:  // fall through
        ASSERT(!CompileTimeValue::IsCompileTimeValue(value));
      case ObjectLiteral::Property::COMPUTED:
        if (key->handle()->IsSymbol()) {
          Visit(value);
          ASSERT_EQ(Expression::kValue, value->context());
          __ pop(rax);
          __ Move(rcx, key->handle());
          Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
          __ call(ic, RelocInfo::CODE_TARGET);
          // StoreIC leaves the receiver on the stack.
          break;
        }
        // fall through
      case ObjectLiteral::Property::PROTOTYPE:
        __ push(rax);
        Visit(key);
        ASSERT_EQ(Expression::kValue, key->context());
        Visit(value);
        ASSERT_EQ(Expression::kValue, value->context());
        __ CallRuntime(Runtime::kSetProperty, 3);
        __ movq(rax, Operand(rsp, 0));  // Restore result into rax.
        break;
      case ObjectLiteral::Property::SETTER:  // fall through
      case ObjectLiteral::Property::GETTER:
        __ push(rax);
        Visit(key);
        ASSERT_EQ(Expression::kValue, key->context());
        __ Push(property->kind() == ObjectLiteral::Property::SETTER ?
                Smi::FromInt(1) :
                Smi::FromInt(0));
        Visit(value);
        ASSERT_EQ(Expression::kValue, value->context());
        __ CallRuntime(Runtime::kDefineAccessor, 4);
        __ movq(rax, Operand(rsp, 0));  // Restore result into rax.
        break;
      default: UNREACHABLE();
    }
  }
  switch (expr->context()) {
    case Expression::kUninitialized:
      UNREACHABLE();
    case Expression::kEffect:
      if (result_saved) __ addq(rsp, Immediate(kPointerSize));
      break;
    case Expression::kValue:
      if (!result_saved) __ push(rax);
      break;
    case Expression::kTest:
      if (result_saved) __ pop(rax);
      TestAndBranch(rax, true_label_, false_label_);
      break;
    case Expression::kValueTest: {
      Label discard;
      if (!result_saved) __ push(rax);
      TestAndBranch(rax, true_label_, &discard);
      __ bind(&discard);
      __ addq(rsp, Immediate(kPointerSize));
      __ jmp(false_label_);
      break;
    }
    case Expression::kTestValue: {
      Label discard;
      if (!result_saved) __ push(rax);
      TestAndBranch(rax, &discard, false_label_);
      __ bind(&discard);
      __ addq(rsp, Immediate(kPointerSize));
      __ jmp(true_label_);
      break;
    }
  }
}


void FastCodeGenerator::VisitArrayLiteral(ArrayLiteral* expr) {
  Comment cmnt(masm_, "[ ArrayLiteral");
  Label make_clone;

  // Fetch the function's literals array.
  __ movq(rbx, Operand(rbp, JavaScriptFrameConstants::kFunctionOffset));
  __ movq(rbx, FieldOperand(rbx, JSFunction::kLiteralsOffset));
  // Check if the literal's boilerplate has been instantiated.
  int offset =
      FixedArray::kHeaderSize + (expr->literal_index() * kPointerSize);
  __ movq(rax, FieldOperand(rbx, offset));
  __ CompareRoot(rax, Heap::kUndefinedValueRootIndex);
  __ j(not_equal, &make_clone);

  // Instantiate the boilerplate.
  __ push(rbx);
  __ Push(Smi::FromInt(expr->literal_index()));
  __ Push(expr->literals());
  __ CallRuntime(Runtime::kCreateArrayLiteralBoilerplate, 3);

  __ bind(&make_clone);
  // Clone the boilerplate.
  __ push(rax);
  if (expr->depth() > 1) {
    __ CallRuntime(Runtime::kCloneLiteralBoilerplate, 1);
  } else {
    __ CallRuntime(Runtime::kCloneShallowLiteralBoilerplate, 1);
  }

  bool result_saved = false;  // Is the result saved to the stack?

  // Emit code to evaluate all the non-constant subexpressions and to store
  // them into the newly cloned array.
  ZoneList<Expression*>* subexprs = expr->values();
  for (int i = 0, len = subexprs->length(); i < len; i++) {
    Expression* subexpr = subexprs->at(i);
    // If the subexpression is a literal or a simple materialized literal it
    // is already set in the cloned array.
    if (subexpr->AsLiteral() != NULL ||
        CompileTimeValue::IsCompileTimeValue(subexpr)) {
      continue;
    }

    if (!result_saved) {
      __ push(rax);
      result_saved = true;
    }
    Visit(subexpr);
    ASSERT_EQ(Expression::kValue, subexpr->context());

    // Store the subexpression value in the array's elements.
    __ pop(rax);  // Subexpression value.
    __ movq(rbx, Operand(rsp, 0));  // Copy of array literal.
    __ movq(rbx, FieldOperand(rbx, JSObject::kElementsOffset));
    int offset = FixedArray::kHeaderSize + (i * kPointerSize);
    __ movq(FieldOperand(rbx, offset), rax);

    // Update the write barrier for the array store.
    __ RecordWrite(rbx, offset, rax, rcx);
  }

  switch (expr->context()) {
    case Expression::kUninitialized:
      UNREACHABLE();
    case Expression::kEffect:
      if (result_saved) __ addq(rsp, Immediate(kPointerSize));
      break;
    case Expression::kValue:
      if (!result_saved) __ push(rax);
      break;
    case Expression::kTest:
      if (result_saved) __ pop(rax);
      TestAndBranch(rax, true_label_, false_label_);
      break;
    case Expression::kValueTest: {
      Label discard;
      if (!result_saved) __ push(rax);
      TestAndBranch(rax, true_label_, &discard);
      __ bind(&discard);
      __ addq(rsp, Immediate(kPointerSize));
      __ jmp(false_label_);
      break;
    }
    case Expression::kTestValue: {
      Label discard;
      if (!result_saved) __ push(rax);
      TestAndBranch(rax, &discard, false_label_);
      __ bind(&discard);
      __ addq(rsp, Immediate(kPointerSize));
      __ jmp(true_label_);
      break;
    }
  }
}


void FastCodeGenerator::VisitAssignment(Assignment* expr) {
  Comment cmnt(masm_, "[ Assignment");
  ASSERT(expr->op() == Token::ASSIGN || expr->op() == Token::INIT_VAR);

  // Record the source position for the assignment.
  SetSourcePosition(expr->position());

  // Left-hand side can only be a property, a global or
  // a (parameter or local) slot.
  Variable* var = expr->target()->AsVariableProxy()->AsVariable();
  Expression* rhs = expr->value();
  if (var == NULL) {
    // Assignment to a property.
    ASSERT(expr->target()->AsProperty() != NULL);
    Property* prop = expr->target()->AsProperty();
    Visit(prop->obj());
    Literal* literal_key = prop->key()->AsLiteral();
    uint32_t dummy;
    if (literal_key != NULL &&
        literal_key->handle()->IsSymbol() &&
        !String::cast(*(literal_key->handle()))->AsArrayIndex(&dummy)) {
      // NAMED property assignment
      Visit(rhs);
      ASSERT_EQ(Expression::kValue, rhs->context());
      __ pop(rax);
      __ Move(rcx, literal_key->handle());
      Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
      __ call(ic, RelocInfo::CODE_TARGET);
      __ nop();
    } else {
      // KEYED property assignment
      Visit(prop->key());
      Visit(rhs);
      ASSERT_EQ(Expression::kValue, rhs->context());
      __ pop(rax);
      Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Initialize));
      __ call(ic, RelocInfo::CODE_TARGET);
      __ nop();
      // Drop key from the stack
      __ addq(rsp, Immediate(kPointerSize));
    }
    // Overwrite the receiver on the stack with the result if needed.
    DropAndMove(expr->context(), rax);
  } else if (var->is_global()) {
    // Assignment to a global variable, use inline caching.  Right-hand-side
    // value is passed in rax, variable name in rcx, and the global object
    // on the stack.

    // Code for the right-hand-side expression depends on its type.
    if (rhs->AsLiteral() != NULL) {
      __ Move(rax, rhs->AsLiteral()->handle());
    } else {
      ASSERT_EQ(Expression::kValue, rhs->context());
      Visit(rhs);
      __ pop(rax);
    }
    // Record position for debugger.
    SetSourcePosition(expr->position());
    __ Move(rcx, var->name());
    __ push(CodeGenerator::GlobalObject());
    Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);
    // Overwrite the global object on the stack with the result if needed.
    DropAndMove(expr->context(), rax);
  } else {
    // Local or parameter assignment.

    // Code for the right-hand-side expression depends on its type.
    if (rhs->AsLiteral() != NULL) {
      // Two cases: 'temp <- (var = constant)', or 'var = constant' with a
      // discarded result.  Always perform the assignment.
      __ Move(kScratchRegister, rhs->AsLiteral()->handle());
      __ movq(Operand(rbp, SlotOffset(var->slot())), kScratchRegister);
      Move(expr->context(), kScratchRegister);
    } else {
      ASSERT_EQ(Expression::kValue, rhs->context());
      Visit(rhs);
      switch (expr->context()) {
        case Expression::kUninitialized:
          UNREACHABLE();
        case Expression::kEffect:
          // Case 'var = temp'.  Discard right-hand-side temporary.
          __ pop(Operand(rbp, SlotOffset(var->slot())));
          break;
        case Expression::kValue:
          // Case 'temp1 <- (var = temp0)'.  Preserve right-hand-side
          // temporary on the stack.
          __ movq(kScratchRegister, Operand(rsp, 0));
          __ movq(Operand(rbp, SlotOffset(var->slot())), kScratchRegister);
          break;
        case Expression::kTest:
          // Case 'if (var = temp) ...'.
          __ pop(rax);
          __ movq(Operand(rbp, SlotOffset(var->slot())), rax);
          TestAndBranch(rax, true_label_, false_label_);
          break;
        case Expression::kValueTest: {
          // Case '(var = temp) || ...' in value context.
          Label discard;
          __ movq(rax, Operand(rsp, 0));
          __ movq(Operand(rbp, SlotOffset(var->slot())), rax);
          TestAndBranch(rax, true_label_, &discard);
          __ bind(&discard);
          __ addq(rsp, Immediate(kPointerSize));
          __ jmp(false_label_);
          break;
        }
        case Expression::kTestValue: {
          // Case '(var = temp) && ...' in value context.
          Label discard;
          __ movq(rax, Operand(rsp, 0));
          __ movq(Operand(rbp, SlotOffset(var->slot())), rax);
          TestAndBranch(rax, &discard, false_label_);
          __ bind(&discard);
          __ addq(rsp, Immediate(kPointerSize));
          __ jmp(true_label_);
          break;
        }
      }
    }
  }
}


void FastCodeGenerator::VisitProperty(Property* expr) {
  Comment cmnt(masm_, "[ Property");
  Expression* key = expr->key();
  uint32_t dummy;

  // Record the source position for the property load.
  SetSourcePosition(expr->position());

  // Evaluate receiver.
  Visit(expr->obj());


  if (key->AsLiteral() != NULL && key->AsLiteral()->handle()->IsSymbol() &&
      !String::cast(*(key->AsLiteral()->handle()))->AsArrayIndex(&dummy)) {
    // Do a NAMED property load.
    // The IC expects the property name in rcx and the receiver on the stack.
    __ Move(rcx, key->AsLiteral()->handle());
    Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
    __ call(ic, RelocInfo::CODE_TARGET);
    // By emitting a nop we make sure that we do not have a "test rax,..."
    // instruction after the call it is treated specially by the LoadIC code.
    __ nop();
  } else {
    // Do a KEYED property load.
    Visit(expr->key());
    Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
    __ call(ic, RelocInfo::CODE_TARGET);
    // By emitting a nop we make sure that we do not have a "test rax,..."
    // instruction after the call it is treated specially by the LoadIC code.
    __ nop();
    // Drop key left on the stack by IC.
    __ addq(rsp, Immediate(kPointerSize));
  }
  DropAndMove(expr->context(), rax);
}


void FastCodeGenerator::EmitCallWithIC(Call* expr, RelocInfo::Mode reloc_info) {
  // Code common for calls using the IC.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    Visit(args->at(i));
    ASSERT_EQ(Expression::kValue, args->at(i)->context());
  }
  // Record source position for debugger.
  SetSourcePosition(expr->position());
  // Call the IC initialization code.
  Handle<Code> ic = CodeGenerator::ComputeCallInitialize(arg_count,
                                                         NOT_IN_LOOP);
  __ call(ic, reloc_info);
  // Restore context register.
  __ movq(rsi, Operand(rbp, StandardFrameConstants::kContextOffset));
  // Discard the function left on TOS.
  DropAndMove(expr->context(), rax);
}


void FastCodeGenerator::EmitCallWithStub(Call* expr) {
  // Code common for calls using the call stub.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    Visit(args->at(i));
  }
  // Record source position for debugger.
  SetSourcePosition(expr->position());
  CallFunctionStub stub(arg_count, NOT_IN_LOOP);
  __ CallStub(&stub);
  // Restore context register.
  __ movq(rsi, Operand(rbp, StandardFrameConstants::kContextOffset));
  // Discard the function left on TOS.
  DropAndMove(expr->context(), rax);
}


void FastCodeGenerator::VisitCall(Call* expr) {
  Expression* fun = expr->expression();

  if (fun->AsProperty() != NULL) {
    // Call on a property.
    Property* prop = fun->AsProperty();
    Literal* key = prop->key()->AsLiteral();
    if (key != NULL && key->handle()->IsSymbol()) {
      // Call on a named property: foo.x(1,2,3)
      __ Push(key->handle());
      Visit(prop->obj());
      // Use call IC
      EmitCallWithIC(expr, RelocInfo::CODE_TARGET);
    } else {
      // Call on a keyed property: foo[key](1,2,3)
      // Use a keyed load IC followed by a call IC.
      Visit(prop->obj());
      Visit(prop->key());
      // Record source position of property.
      SetSourcePosition(prop->position());
      Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
      __ call(ic, RelocInfo::CODE_TARGET);
      // By emitting a nop we make sure that we do not have a "test eax,..."
      // instruction after the call it is treated specially by the LoadIC code.
      __ nop();
      // Drop key left on the stack by IC.
      __ addq(rsp, Immediate(kPointerSize));
      // Pop receiver.
      __ pop(rbx);
      // Push result (function).
      __ push(rax);
      // Push receiver object on stack.
      if (prop->is_synthetic()) {
        __ push(CodeGenerator::GlobalObject());
      } else {
        __ push(rbx);
      }
      EmitCallWithStub(expr);
    }
  } else if (fun->AsVariableProxy()->AsVariable() != NULL) {
    // Call on a global variable
    Variable* var = fun->AsVariableProxy()->AsVariable();
    ASSERT(var != NULL && !var->is_this() && var->is_global());
    ASSERT(!var->is_possibly_eval());
    __ Push(var->name());
    // Push global object (receiver).
    __ push(CodeGenerator::GlobalObject());
    EmitCallWithIC(expr, RelocInfo::CODE_TARGET_CONTEXT);
  } else {
    // Calls we cannot handle right now.
    // Should bailout in the CodeGenSelector.
    UNREACHABLE();
  }
}


void FastCodeGenerator::VisitCallNew(CallNew* expr) {
  Comment cmnt(masm_, "[ CallNew");
  // According to ECMA-262, section 11.2.2, page 44, the function
  // expression in new calls must be evaluated before the
  // arguments.
  // Push function on the stack.
  Visit(expr->expression());
  ASSERT_EQ(Expression::kValue, expr->expression()->context());
  // If location is value, already on the stack,

  // Push global object (receiver).
  __ push(CodeGenerator::GlobalObject());

  // Push the arguments ("left-to-right") on the stack.
  ZoneList<Expression*>* args = expr->arguments();
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    Visit(args->at(i));
    ASSERT_EQ(Expression::kValue, args->at(i)->context());
    // If location is value, it is already on the stack,
    // so nothing to do here.
  }

  // Call the construct call builtin that handles allocation and
  // constructor invocation.
  SetSourcePosition(expr->position());

  // Load function, arg_count into rdi and rax.
  __ Set(rax, arg_count);
  // Function is in rsp[arg_count + 1].
  __ movq(rdi, Operand(rsp, rax, times_pointer_size, kPointerSize));

  Handle<Code> construct_builtin(Builtins::builtin(Builtins::JSConstructCall));
  __ Call(construct_builtin, RelocInfo::CONSTRUCT_CALL);

  // Replace function on TOS with result in rax, or pop it.
  DropAndMove(expr->context(), rax);
}


void FastCodeGenerator::VisitCallRuntime(CallRuntime* expr) {
  Comment cmnt(masm_, "[ CallRuntime");
  ZoneList<Expression*>* args = expr->arguments();
  Runtime::Function* function = expr->function();

  ASSERT(function != NULL);

  // Push the arguments ("left-to-right").
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    Visit(args->at(i));
    ASSERT_EQ(Expression::kValue, args->at(i)->context());
  }

  __ CallRuntime(function, arg_count);
  Move(expr->context(), rax);
}


void FastCodeGenerator::VisitUnaryOperation(UnaryOperation* expr) {
  Comment cmnt(masm_, "[ UnaryOperation");

  switch (expr->op()) {
    case Token::VOID:
      Visit(expr->expression());
      ASSERT_EQ(Expression::kEffect, expr->expression()->context());
      switch (expr->context()) {
        case Expression::kUninitialized:
          UNREACHABLE();
          break;
        case Expression::kEffect:
          break;
        case Expression::kValue:
          __ PushRoot(Heap::kUndefinedValueRootIndex);
          break;
        case Expression::kTestValue:
          // Value is false so it's needed.
          __ PushRoot(Heap::kUndefinedValueRootIndex);
          // Fall through.
        case Expression::kTest:  // Fall through.
        case Expression::kValueTest:
          __ jmp(false_label_);
          break;
      }
      break;

    default:
      UNREACHABLE();
  }
}


void FastCodeGenerator::VisitBinaryOperation(BinaryOperation* expr) {
  switch (expr->op()) {
    case Token::COMMA:
      ASSERT_EQ(Expression::kEffect, expr->left()->context());
      ASSERT_EQ(expr->context(), expr->right()->context());
      Visit(expr->left());
      Visit(expr->right());
      break;

    case Token::OR:
    case Token::AND:
      EmitLogicalOperation(expr);
      break;

    case Token::ADD:
    case Token::SUB:
    case Token::DIV:
    case Token::MOD:
    case Token::MUL:
    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR:
    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      ASSERT_EQ(Expression::kValue, expr->left()->context());
      ASSERT_EQ(Expression::kValue, expr->right()->context());

      Visit(expr->left());
      Visit(expr->right());
      GenericBinaryOpStub stub(expr->op(),
                               NO_OVERWRITE,
                               NO_GENERIC_BINARY_FLAGS);
      __ CallStub(&stub);
      Move(expr->context(), rax);

      break;
    }
    default:
      UNREACHABLE();
  }
}


} }  // namespace v8::internal
