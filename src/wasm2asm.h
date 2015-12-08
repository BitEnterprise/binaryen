
//
// WebAssembly-to-asm.js translator. Uses the Emscripten optimizer
// infrastructure.
//

#include "wasm.h"
#include "emscripten-optimizer/optimizer.h"
#include "mixed_arena.h"
#include "asm_v_wasm.h"
#include "shared-constants.h"

namespace wasm {

extern int debug;

using namespace cashew;

IString ASM_FUNC("asmFunc"),
        ABORT_FUNC("abort"),
        FUNCTION_TABLE("FUNCTION_TABLE"),
        NO_RESULT("wasm2asm$noresult"), // no result at all
        EXPRESSION_RESULT("wasm2asm$expresult"); // result in an expression, no temp var

// Appends extra to block, flattening out if extra is a block as well
void flattenAppend(Ref ast, Ref extra) {
  int index;
  if (ast[0] == BLOCK) index = 1;
  else if (ast[0] == DEFUN) index = 3;
  else abort();
  if (extra[0] == BLOCK) {
    for (int i = 0; i < extra[1]->size(); i++) {
      ast[index]->push_back(extra[1][i]);
    }
  } else {
    ast[index]->push_back(extra);
  }
}

//
// Wasm2AsmBuilder - converts a WebAssembly module into asm.js
//
// In general, asm.js => wasm is very straightforward, as can
// be seen in asm2wasm.h. Just a single pass, plus a little
// state bookkeeping (breakStack, etc.), and a few after-the
// fact corrections for imports, etc. However, wasm => asm.js
// is tricky because wasm has statements == expressions, or in
// other words, things like `break` and `if` can show up
// in places where asm.js can't handle them, like inside an
// a loop's condition check.
//
// We therefore need the ability to lower an expression into
// a block of statements, and we keep statementizing until we
// reach a context in which we can emit those statments. This
// requires that we create temp variables to store values
// that would otherwise flow directly into their targets if
// we were an expression (e.g. if a loop's condition check
// is a bunch of statements, we execute those statements,
// then use the computed value in the loop's condition;
// we might also be able to avoid an assign to a temp var
// at the end of those statements, and put just that
// value in the loop's condition).
//
// It is possible to do this in a single pass, if we just
// allocate temp vars freely. However, pathological cases
// can easily show bad behavior here, with many unnecessary
// temp vars. We could rely on optimization passes like
// Emscripten's eliminate/registerize pair, but we want
// wasm2asm to be fairly fast to run, as it might run on
// the client.
//
// The approach taken here therefore performs 2 passes on
// each function. First, it finds which expression will need to
// be statementized. It also sees which labels can receive a break
// with a value. Given that information, in the second pass we can
// allocate // temp vars in an efficient manner, as we know when we
// need them and when their use is finished. They are allocated
// using an RAII class, so that they are automatically freed
// when the scope ends. This means that a node cannot allocate
// its own temp var; instead, the parent - which knows the
// child will return a value in a temp var - allocates it,
// and tells the child what temp var to emit to. The child
// can then pass forward that temp var to its children,
// optimizing away unnecessary forwarding.


class Wasm2AsmBuilder {
public:
  Ref processWasm(Module* wasm);
  Ref processFunction(Function* func);

  // The first pass on an expression: scan it to see whether it will
  // need to be statementized, and note spooky returns of values at
  // a distance (aka break with a value).
  void scanFunctionBody(Expression* curr);

  // The second pass on an expression: process it fully, generating
  // asm.js
  // @param result Whether the context we are in receives a value,
  //               and its type, or if not, then we can drop our return,
  //               if we have one.
  Ref processFunctionBody(Expression* curr, IString result);

  // Get a temp var.
  IString getTemp(WasmType type) {
    IString ret;
    if (frees[type].size() > 0) {
      ret = frees[type].back();
      frees[type].pop_back();
    } else {
      size_t index = temps[type]++;
      ret = IString((std::string("wasm2asm_") + printWasmType(type) + "$" + std::to_string(index)).c_str(), false);
    }
    return ret;
  }
  // Free a temp var.
  void freeTemp(WasmType type, IString temp) {
    frees[type].push_back(temp);
  }

  static IString fromName(Name name) {
    // TODO: more clever name fixing, including checking we do not collide
    const char *str = name.str;
    // check the various issues, and recurse so we check the others
    if (strchr(str, '-')) {
      char *mod = strdup(str); // XXX leak
      str = mod;
      while (*mod) {
        if (*mod == '-') *mod = '_';
        mod++;
      }
      return fromName(IString(str, false));
    }
    if (isdigit(str[0])) {
      std::string prefixed = "$$";
      prefixed += name.str;
      return fromName(IString(prefixed.c_str(), false));
    }
    return name;
  }

  void setStatement(Expression* curr) {
    willBeStatement.insert(curr);
  }
  bool isStatement(Expression* curr) {
    return curr && willBeStatement.find(curr) != willBeStatement.end();
  }

  size_t getTableSize() {
    return tableSize;
  }

private:
  // How many temp vars we need
  std::vector<int> temps; // type => num temps
  // Which are currently free to use
  std::vector<std::vector<IString>> frees; // type => list of free names

  // Expressions that will be a statement.
  std::set<Expression*> willBeStatement;

  // All our function tables have the same size TODO: optimize?
  size_t tableSize;

  void addBasics(Ref ast);
  void addImport(Ref ast, Import *import);
  void addTables(Ref ast, Module *wasm);
  void addExports(Ref ast, Module *wasm);
};

Ref Wasm2AsmBuilder::processWasm(Module* wasm) {
  Ref ret = ValueBuilder::makeToplevel();
  Ref asmFunc = ValueBuilder::makeFunction(ASM_FUNC);
  ret[1]->push_back(asmFunc);
  ValueBuilder::appendArgumentToFunction(asmFunc, GLOBAL);
  ValueBuilder::appendArgumentToFunction(asmFunc, ENV);
  ValueBuilder::appendArgumentToFunction(asmFunc, BUFFER);
  asmFunc[3]->push_back(ValueBuilder::makeStatement(ValueBuilder::makeString(USE_ASM)));
  // create heaps, etc
  addBasics(asmFunc[3]);
  for (auto import : wasm->imports) {
    addImport(asmFunc[3], import);
  }
  // figure out the table size
  tableSize = wasm->table.names.size();
  size_t pow2ed = 1;
  while (pow2ed < tableSize) {
    pow2ed <<= 1;
  }
  tableSize = pow2ed;
  // functions
  for (auto func : wasm->functions) {
    asmFunc[3]->push_back(processFunction(func));
  }
  addTables(asmFunc[3], wasm);
  // memory XXX
  addExports(asmFunc[3], wasm);
  return ret;
}

void Wasm2AsmBuilder::addBasics(Ref ast) {
  // heaps, var HEAP8 = new global.Int8Array(buffer); etc
  auto addHeap = [&](IString name, IString view) {
    Ref theVar = ValueBuilder::makeVar();
    ast->push_back(theVar);
    ValueBuilder::appendToVar(theVar,
      name,
      ValueBuilder::makeNew(
        ValueBuilder::makeCall(
          ValueBuilder::makeDot(
            ValueBuilder::makeName(GLOBAL),
            view
          ),
          ValueBuilder::makeName(BUFFER)
        )
      )
    );
  };
  addHeap(HEAP8,  INT8ARRAY);
  addHeap(HEAP16, INT16ARRAY);
  addHeap(HEAP32, INT32ARRAY);
  addHeap(HEAPU8,  UINT8ARRAY);
  addHeap(HEAPU16, UINT16ARRAY);
  addHeap(HEAPU32, UINT32ARRAY);
  addHeap(HEAPF32, FLOAT32ARRAY);
  addHeap(HEAPF64, FLOAT64ARRAY);
  // core asm.js imports
  auto addMath = [&](IString name, IString base) {
    Ref theVar = ValueBuilder::makeVar();
    ast->push_back(theVar);
    ValueBuilder::appendToVar(theVar,
      name,
      ValueBuilder::makeDot(
        ValueBuilder::makeDot(
          ValueBuilder::makeName(GLOBAL),
          MATH
        ),
        base
      )
    );
  };
  addMath(MATH_IMUL, IMUL);
  addMath(MATH_FROUND, FROUND);
  addMath(MATH_ABS, ABS);
  addMath(MATH_CLZ32, CLZ32);
}

void Wasm2AsmBuilder::addImport(Ref ast, Import *import) {
  Ref theVar = ValueBuilder::makeVar();
  ast->push_back(theVar);
  Ref module = ValueBuilder::makeName(ENV); // TODO: handle nested module imports
  ValueBuilder::appendToVar(theVar,
    fromName(import->name),
    ValueBuilder::makeDot(
      module,
      fromName(import->base)
    )
  );
}

void Wasm2AsmBuilder::addTables(Ref ast, Module *wasm) {
  std::map<std::string, std::vector<IString>> tables; // asm.js tables, sig => contents of table
  for (size_t i = 0; i < wasm->table.names.size(); i++) {
    Name name = wasm->table.names[i];
    auto func = wasm->functionsMap[name];
    std::string sig = getSig(func);
    auto& table = tables[sig];
    if (table.size() == 0) {
      // fill it with the first of its type seen. we have to fill with something; and for asm2wasm output, the first is the null anyhow
      table.resize(tableSize);
      for (int j = 0; j < tableSize; j++) {
        table[j] = fromName(name);
      }
    } else {
      table[i] = fromName(name);
    }
  }
  for (auto& pair : tables) {
    auto& sig = pair.first;
    auto& table = pair.second;
    std::string stable = std::string("FUNCTION_TABLE_") + sig;
    IString asmName = IString(stable.c_str(), false);
    // add to asm module
    Ref theVar = ValueBuilder::makeVar();
    ast->push_back(theVar);
    Ref theArray = ValueBuilder::makeArray();
    ValueBuilder::appendToVar(theVar, asmName, theArray);
    for (auto& name : table) {
      ValueBuilder::appendToArray(theArray, ValueBuilder::makeName(name));
    }
  }
}

void Wasm2AsmBuilder::addExports(Ref ast, Module *wasm) {
  Ref exports = ValueBuilder::makeObject();
  for (auto export_ : wasm->exports) {
    ValueBuilder::appendToObject(exports, fromName(export_->name), ValueBuilder::makeName(fromName(export_->value)));
  }
  ast->push_back(ValueBuilder::makeStatement(ValueBuilder::makeReturn(exports)));
}

Ref Wasm2AsmBuilder::processFunction(Function* func) {
  if (debug) std::cerr << "  processFunction " << func->name << '\n';
  Ref ret = ValueBuilder::makeFunction(fromName(func->name));
  frees.clear();
  frees.resize(std::max(i32, std::max(f32, f64)) + 1);
  temps.clear();
  temps.resize(std::max(i32, std::max(f32, f64)) + 1);
  temps[i32] = temps[f32] = temps[f64] = 0;
  // arguments
  for (auto& param : func->params) {
    IString name = fromName(param.name);
    ValueBuilder::appendArgumentToFunction(ret, name);
    ret[3]->push_back(
      ValueBuilder::makeStatement(
        ValueBuilder::makeAssign(
          ValueBuilder::makeName(name),
          makeAsmCoercion(ValueBuilder::makeName(name), wasmToAsmType(param.type))
        )
      )
    );
  }
  Ref theVar = ValueBuilder::makeVar();
  size_t theVarIndex = ret[3]->size();
  ret[3]->push_back(theVar);
  // body
  scanFunctionBody(func->body);
  if (isStatement(func->body)) {
    IString result = func->result != none ? getTemp(func->result) : NO_RESULT;
    flattenAppend(ret, ValueBuilder::makeStatement(processFunctionBody(func->body, result)));
    if (func->result != none) {
      // do the actual return
      ret[3]->push_back(ValueBuilder::makeStatement(ValueBuilder::makeReturn(makeAsmCoercion(ValueBuilder::makeName(result), wasmToAsmType(func->result)))));
      freeTemp(func->result, result);
    }
  } else {
    // whole thing is an expression, just do a return
    if (func->result != none) {
      ret[3]->push_back(ValueBuilder::makeStatement(ValueBuilder::makeReturn(makeAsmCoercion(processFunctionBody(func->body, EXPRESSION_RESULT), wasmToAsmType(func->result)))));
    } else {
      flattenAppend(ret, processFunctionBody(func->body, NO_RESULT));
    }
  }
  // locals, including new temp locals
  for (auto& local : func->locals) {
    ValueBuilder::appendToVar(theVar, fromName(local.name), makeAsmCoercedZero(wasmToAsmType(local.type)));
  }
  for (auto f : frees[i32]) {
    ValueBuilder::appendToVar(theVar, f, makeAsmCoercedZero(ASM_INT));
  }
  for (auto f : frees[f32]) {
    ValueBuilder::appendToVar(theVar, f, makeAsmCoercedZero(ASM_FLOAT));
  }
  for (auto f : frees[f64]) {
    ValueBuilder::appendToVar(theVar, f, makeAsmCoercedZero(ASM_DOUBLE));
  }
  if (theVar[1]->size() == 0) {
    ret[3]->splice(theVarIndex, 1);
  }
  // checks
  assert(frees[i32].size() == temps[i32]); // all temp vars should be free at the end
  assert(frees[f32].size() == temps[f32]); // all temp vars should be free at the end
  assert(frees[f64].size() == temps[f64]); // all temp vars should be free at the end
  // cleanups
  willBeStatement.clear();
  return ret;
}

void Wasm2AsmBuilder::scanFunctionBody(Expression* curr) {
  struct ExpressionScanner : public WasmWalker {
    Wasm2AsmBuilder* parent;

    ExpressionScanner(Wasm2AsmBuilder* parent) : parent(parent) {}

    // Visitors

    void visitBlock(Block *curr) override {
      parent->setStatement(curr);
    }
    void visitIf(If *curr) override {
      parent->setStatement(curr);
    }
    void visitLoop(Loop *curr) override {
      parent->setStatement(curr);
    }
    void visitLabel(Label *curr) override {
      parent->setStatement(curr);
    }
    void visitBreak(Break *curr) override {
      parent->setStatement(curr);
    }
    void visitSwitch(Switch *curr) override {
      parent->setStatement(curr);
    }
    void visitCall(Call *curr) override {
      for (auto item : curr->operands) {
        if (parent->isStatement(item)) {
          parent->setStatement(curr);
          break;
        }
      }
    }
    void visitCallImport(CallImport *curr) override {
      visitCall(curr);
    }
    void visitCallIndirect(CallIndirect *curr) override {
      if (parent->isStatement(curr->target)) {
        parent->setStatement(curr);
        return;
      }
      for (auto item : curr->operands) {
        if (parent->isStatement(item)) {
          parent->setStatement(curr);
          break;
        }
      }
    }
    void visitSetLocal(SetLocal *curr) override {
      if (parent->isStatement(curr->value)) {
        parent->setStatement(curr);
      }
    }
    void visitLoad(Load *curr) override {
      if (parent->isStatement(curr->ptr)) {
        parent->setStatement(curr);
      }
    }
    void visitStore(Store *curr) override {
      if (parent->isStatement(curr->ptr) || parent->isStatement(curr->value)) {
        parent->setStatement(curr);
      }
    }
    void visitUnary(Unary *curr) override {
      if (parent->isStatement(curr->value)) {
        parent->setStatement(curr);
      }
    }
    void visitBinary(Binary *curr) override {
      if (parent->isStatement(curr->left) || parent->isStatement(curr->right)) {
        parent->setStatement(curr);
      }
    }
    void visitSelect(Select *curr) override {
      if (parent->isStatement(curr->condition) || parent->isStatement(curr->ifTrue) || parent->isStatement(curr->ifFalse)) {
        parent->setStatement(curr);
      }
    }
    void visitHost(Host *curr) override {
      for (auto item : curr->operands) {
        if (parent->isStatement(item)) {
          parent->setStatement(curr);
          break;
        }
      }
    }
  };
  ExpressionScanner(this).walk(curr);
}

Ref Wasm2AsmBuilder::processFunctionBody(Expression* curr, IString result) {
  struct ExpressionProcessor : public WasmVisitor<Ref> {
    Wasm2AsmBuilder* parent;
    IString result;
    ExpressionProcessor(Wasm2AsmBuilder* parent) : parent(parent) {}

    // A scoped temporary variable.
    struct ScopedTemp {
      Wasm2AsmBuilder* parent;
      WasmType type;
      IString temp;
      bool needFree;
      // @param possible if provided, this is a variable we can use as our temp. it has already been
      //                 allocated in a higher scope, and we can just assign to it as our result is
      //                 going there anyhow.
      ScopedTemp(WasmType type, Wasm2AsmBuilder* parent, IString possible = NO_RESULT) : parent(parent), type(type) {
        assert(possible != EXPRESSION_RESULT);
        if (possible == NO_RESULT) {
          temp = parent->getTemp(type);
          needFree = true;
        } else {
          temp = possible;
          needFree = false;
        }
      }
      ~ScopedTemp() {
        if (needFree) {
          parent->freeTemp(type, temp);
        }
      }

      IString getName() {
        return temp;
      }
      Ref getAstName() {
        return ValueBuilder::makeName(temp);
      }
    };

    Ref visit(Expression* curr, IString nextResult) {
      IString old = result;
      result = nextResult;
      Ref ret = WasmVisitor::visit(curr);
      result = old; // keep it consistent for the rest of this frame, which may call visit on multiple children
      return ret;
    }

    Ref visit(Expression* curr, ScopedTemp& temp) {
      return visit(curr, temp.temp);
    }

    Ref visitForExpression(Expression* curr, WasmType type, IString& tempName) { // this result is for an asm expression slot, but it might be a statement
      if (isStatement(curr)) {
        ScopedTemp temp(type, parent);
        tempName = temp.temp;
        return visit(curr, temp);
      } else {
        return visit(curr, EXPRESSION_RESULT);
      }
    }

    Ref visitAndAssign(Expression* curr, IString result) {
      Ref ret = visit(curr, result);
      // if it's not already a statement, then it's an expression, and we need to assign it
      // (if it is a statement, it already assigns to the result var)
      if (!isStatement(curr) && result != NO_RESULT) {
        ret = ValueBuilder::makeStatement(ValueBuilder::makeAssign(ValueBuilder::makeName(result), ret));
      }
      return ret;
    }

    Ref visitAndAssign(Expression* curr, ScopedTemp& temp) {
      return visitAndAssign(curr, temp.getName());
    }

    bool isStatement(Expression* curr) {
      return parent->isStatement(curr);
    }

    // Expressions with control flow turn into a block, which we must
    // then handle, even if we are an expression.
    bool isBlock(Ref ast) {
      return !!ast && ast[0] == BLOCK;
    }

    Ref blockify(Ref ast) {
      if (isBlock(ast)) return ast;
      Ref ret = ValueBuilder::makeBlock();
      ret[1]->push_back(ValueBuilder::makeStatement(ast));
      return ret;
    }

    // For spooky return-at-a-distance/break-with-result, this tells us
    // what the result var is for a specific label.
    std::map<Name, IString> breakResults;

    // Breaks to the top of a loop should be emitted as continues, to that loop's main label
    std::map<Name, Name> continueLabels;

    IString fromName(Name name) {
      return parent->fromName(name);
    }

    // Visitors

    Ref visitBlock(Block *curr) override {
      breakResults[curr->name] = result;
      Ref ret = ValueBuilder::makeBlock();
      size_t size = curr->list.size();
      int noResults = result == NO_RESULT ? size : size-1;
      for (size_t i = 0; i < noResults; i++) {
        flattenAppend(ret, ValueBuilder::makeStatement(visit(curr->list[i], NO_RESULT)));
      }
      if (result != NO_RESULT) {
        flattenAppend(ret, visitAndAssign(curr->list[size-1], result));
      }
      if (curr->name.is()) {
        ret = ValueBuilder::makeLabel(fromName(curr->name), ret);
      }
      return ret;
    }
    Ref visitIf(If *curr) override {
      IString temp;
      Ref condition = visitForExpression(curr->condition, i32, temp);
      Ref ifTrue = ValueBuilder::makeStatement(visitAndAssign(curr->ifTrue, result));
      Ref ifFalse;
      if (curr->ifFalse) {
        ifFalse = ValueBuilder::makeStatement(visitAndAssign(curr->ifFalse, result));
      }
      if (temp.isNull()) {
        return ValueBuilder::makeIf(condition, ifTrue, ifFalse); // simple if
      }
      condition = blockify(condition);
      // just add an if to the block
      condition[1]->push_back(ValueBuilder::makeIf(ValueBuilder::makeName(temp), ifTrue, ifFalse));
      return condition;
    }
    Ref visitLoop(Loop *curr) override {
      Name asmLabel = curr->out.is() ? curr->out : curr->in; // label using the outside, normal for breaks. if no outside, then inside
      if (curr->in.is()) continueLabels[curr->in] = asmLabel;
      Ref body = visit(curr->body, result);
      Ref ret = ValueBuilder::makeDo(body, ValueBuilder::makeInt(0));
      if (asmLabel.is()) {
        ret = ValueBuilder::makeLabel(fromName(asmLabel), ret);
      }
      return ret;
    }
    Ref visitLabel(Label *curr) override {
      return ValueBuilder::makeLabel(fromName(curr->name), visit(curr->body, result));
    }
    Ref visitBreak(Break *curr) override {
      if (curr->condition) {
        // we need an equivalent to an if here, so use that code
        Break fakeBreak = *curr;
        fakeBreak.condition = nullptr;
        If fakeIf;
        fakeIf.condition = curr->condition;
        fakeIf.ifTrue = &fakeBreak;
        return visit(&fakeIf, result);
      }
      Ref theBreak;
      auto iter = continueLabels.find(curr->name);
      if (iter == continueLabels.end()) {
        theBreak = ValueBuilder::makeBreak(fromName(curr->name));
      } else {
        theBreak = ValueBuilder::makeContinue(fromName(iter->second));
      }
      if (!curr->value) return theBreak;
      // generate the value, including assigning to the result, and then do the break
      Ref ret = visitAndAssign(curr->value, breakResults[curr->name]);
      ret = blockify(ret);
      ret[1]->push_back(theBreak);
      return ret;
    }
    Ref visitSwitch(Switch *curr) override {
      Ref ret = ValueBuilder::makeLabel(fromName(curr->name), ValueBuilder::makeBlock());
      Ref value;
      if (isStatement(curr->value)) {
        ScopedTemp temp(i32, parent);
        flattenAppend(ret[2], visit(curr->value, temp));
        value = temp.getAstName();
      } else {
        value = visit(curr->value, EXPRESSION_RESULT);
      }
      Ref theSwitch = ValueBuilder::makeSwitch(value);
      ret[2][1]->push_back(theSwitch);
      for (auto& c : curr->cases) {
        bool added = false;
        for (size_t i = 0; i < curr->targets.size(); i++) {
          if (curr->targets[i] == c.name) {
            ValueBuilder::appendCaseToSwitch(theSwitch, ValueBuilder::makeNum(i));
            added = true;
          }
        }
        if (c.name == curr->default_) {
          ValueBuilder::appendDefaultToSwitch(theSwitch);
          added = true;
        }
        assert(added);
        ValueBuilder::appendCodeToSwitch(theSwitch, blockify(visit(c.body, NO_RESULT)), false);
      }
      return ret;
    }

    Ref makeStatementizedCall(ExpressionList& operands, Ref ret, Ref theCall, IString result, WasmType type) {
      std::vector<ScopedTemp*> temps; // TODO: utility class, with destructor?
      for (auto& operand : operands) {
        temps.push_back(new ScopedTemp(operand->type, parent));
        IString temp = temps.back()->temp;
        flattenAppend(ret, visitAndAssign(operand, temp));
        theCall[2]->push_back(makeAsmCoercion(ValueBuilder::makeName(temp), wasmToAsmType(operand->type)));
      }
      theCall = makeAsmCoercion(theCall, wasmToAsmType(type));
      if (result != NO_RESULT) {
        theCall = ValueBuilder::makeStatement(ValueBuilder::makeAssign(ValueBuilder::makeName(result), theCall));
      }
      flattenAppend(ret, theCall);
      for (auto temp : temps) {
        delete temp;
      }
      return ret;
    }

    Ref visitCall(Call *curr) override {
      Ref theCall = ValueBuilder::makeCall(fromName(curr->target));
      if (!isStatement(curr)) {
        // none of our operands is a statement; go right ahead and create a simple expression
        for (auto operand : curr->operands) {
          theCall[2]->push_back(makeAsmCoercion(visit(operand, EXPRESSION_RESULT), wasmToAsmType(operand->type)));
        }
        return makeAsmCoercion(theCall, wasmToAsmType(curr->type));
      }
      // we must statementize them all
      return makeStatementizedCall(curr->operands, ValueBuilder::makeBlock(), theCall, result, curr->type);
    }
    Ref visitCallImport(CallImport *curr) override {
      return visitCall(curr);
    }
    Ref visitCallIndirect(CallIndirect *curr) override {
      std::string stable = std::string("FUNCTION_TABLE_") + getSig(curr->fullType);
      IString table = IString(stable.c_str(), false);
      auto makeTableCall = [&](Ref target) {
        return ValueBuilder::makeCall(ValueBuilder::makeSub(
          ValueBuilder::makeName(table),
          ValueBuilder::makeBinary(target, AND, ValueBuilder::makeInt(parent->getTableSize()-1))
        ));
      };
      if (!isStatement(curr)) {
        // none of our operands is a statement; go right ahead and create a simple expression
        Ref theCall = makeTableCall(visit(curr->target, EXPRESSION_RESULT));
        for (auto operand : curr->operands) {
          theCall[2]->push_back(makeAsmCoercion(visit(operand, EXPRESSION_RESULT), wasmToAsmType(operand->type)));
        }
        return makeAsmCoercion(theCall, wasmToAsmType(curr->type));
      }
      // we must statementize them all
      Ref ret = ValueBuilder::makeBlock();
      ScopedTemp temp(i32, parent);
      flattenAppend(ret, visit(curr->target, temp));
      Ref theCall = makeTableCall(temp.getAstName());
      return makeStatementizedCall(curr->operands, ret, theCall, result, curr->type);
    }
    Ref visitGetLocal(GetLocal *curr) override {
      return ValueBuilder::makeName(fromName(curr->name));
    }
    Ref visitSetLocal(SetLocal *curr) override {
      if (!isStatement(curr)) {
        return ValueBuilder::makeAssign(ValueBuilder::makeName(fromName(curr->name)), visit(curr->value, EXPRESSION_RESULT));
      }
      ScopedTemp temp(curr->type, parent, result); // if result was provided, our child can just assign there. otherwise, allocate a temp for it to assign to.
      Ref ret = blockify(visit(curr->value, temp));
      // the output was assigned to result, so we can just assign it to our target
      ret[1]->push_back(ValueBuilder::makeStatement(ValueBuilder::makeAssign(ValueBuilder::makeName(fromName(curr->name)), temp.getAstName())));
      return ret;
    }
    Ref visitLoad(Load *curr) override {
      if (isStatement(curr)) {
        ScopedTemp temp(i32, parent);
        GetLocal fakeLocal;
        fakeLocal.name = temp.getName();
        Load fakeLoad = *curr;
        fakeLoad.ptr = &fakeLocal;
        Ref ret = blockify(visitAndAssign(curr->ptr, temp));
        flattenAppend(ret, visitAndAssign(&fakeLoad, result));
        return ret;
      }
      // normal load
      assert(curr->bytes == curr->align); // TODO: unaligned
      Ref ptr = visit(curr->ptr, EXPRESSION_RESULT);
      Ref ret;
      switch (curr->type) {
        case i32: {
          switch (curr->bytes) {
            case 1: ret = ValueBuilder::makeSub(ValueBuilder::makeName(curr->signed_ ? HEAP8  : HEAPU8 ), ValueBuilder::makePtrShift(ptr, 0)); break;
            case 2: ret = ValueBuilder::makeSub(ValueBuilder::makeName(curr->signed_ ? HEAP16 : HEAPU16), ValueBuilder::makePtrShift(ptr, 1)); break;
            case 4: ret = ValueBuilder::makeSub(ValueBuilder::makeName(curr->signed_ ? HEAP32 : HEAPU32), ValueBuilder::makePtrShift(ptr, 2)); break;
            default: abort();
          }
          break;
        }
        case f32: ret = ValueBuilder::makeSub(ValueBuilder::makeName(HEAPF32), ValueBuilder::makePtrShift(ptr, 2)); break;
        case f64: ret = ValueBuilder::makeSub(ValueBuilder::makeName(HEAPF64), ValueBuilder::makePtrShift(ptr, 3)); break;
        default: abort();
      }
      return makeAsmCoercion(ret, wasmToAsmType(curr->type));
    }
    Ref visitStore(Store *curr) override {
      if (isStatement(curr)) {
        ScopedTemp tempPtr(i32, parent);
        ScopedTemp tempValue(curr->type, parent);
        GetLocal fakeLocalPtr;
        fakeLocalPtr.name = tempPtr.getName();
        GetLocal fakeLocalValue;
        fakeLocalValue.name = tempValue.getName();
        Store fakeStore = *curr;
        fakeStore.ptr = &fakeLocalPtr;
        fakeStore.value = &fakeLocalValue;
        Ref ret = blockify(visitAndAssign(curr->ptr, tempPtr));
        flattenAppend(ret, visitAndAssign(curr->value, tempValue));
        flattenAppend(ret, visitAndAssign(&fakeStore, result));
        return ret;
      }
      // normal store
      assert(curr->bytes == curr->align); // TODO: unaligned, i64
      Ref ptr = visit(curr->ptr, EXPRESSION_RESULT);
      Ref value = visit(curr->value, EXPRESSION_RESULT);
      Ref ret;
      switch (curr->type) {
        case i32: {
          switch (curr->bytes) {
            case 1: ret = ValueBuilder::makeSub(ValueBuilder::makeName(HEAP8),  ValueBuilder::makePtrShift(ptr, 0)); break;
            case 2: ret = ValueBuilder::makeSub(ValueBuilder::makeName(HEAP16), ValueBuilder::makePtrShift(ptr, 1)); break;
            case 4: ret = ValueBuilder::makeSub(ValueBuilder::makeName(HEAP32), ValueBuilder::makePtrShift(ptr, 2)); break;
            default: abort();
          }
          break;
        }
        case f32: ret = ValueBuilder::makeSub(ValueBuilder::makeName(HEAPF32), ValueBuilder::makePtrShift(ptr, 2)); break;
        case f64: ret = ValueBuilder::makeSub(ValueBuilder::makeName(HEAPF64), ValueBuilder::makePtrShift(ptr, 3)); break;
        default: abort();
      }
      return ValueBuilder::makeAssign(ret, value);
    }
    Ref visitConst(Const *curr) override {
      switch (curr->type) {
        case i32: return ValueBuilder::makeInt(curr->value.i32);
        // TODO: i64. statementize?
        case f32: {
          Ref ret = ValueBuilder::makeCall(MATH_FROUND);
          Const fake;
          fake.value = double(curr->value.f32);
          fake.type = f64;
          ret[2]->push_back(visitConst(&fake));
          return ret;
        }
        case f64: {
          double d = curr->value.f64;
          if (d == 0 && 1/d < 0) { // negative zero
            return ValueBuilder::makeUnary(PLUS, ValueBuilder::makeUnary(MINUS, ValueBuilder::makeDouble(0)));
          }
          return ValueBuilder::makeUnary(PLUS, ValueBuilder::makeDouble(curr->value.f64));
        }
        default: abort();
      }
    }
    Ref visitUnary(Unary *curr) override {
      if (isStatement(curr)) {
        ScopedTemp temp(curr->value->type, parent);
        GetLocal fakeLocal;
        fakeLocal.name = temp.getName();
        Unary fakeUnary = *curr;
        fakeUnary.value = &fakeLocal;
        Ref ret = blockify(visitAndAssign(curr->value, temp));
        flattenAppend(ret, visitAndAssign(&fakeUnary, result));
        return ret;
      }
      // normal unary
      Ref value = visit(curr->value, EXPRESSION_RESULT);
      switch (curr->type) {
        case i32: {
          switch (curr->op) {
            case Clz:     return ValueBuilder::makeCall(MATH_CLZ32, value);
            case Ctz:     return ValueBuilder::makeCall(MATH_CTZ32, value);
            case Popcnt:  return ValueBuilder::makeCall(MATH_POPCNT32, value);
            default: abort();
          }
        }
        case f32:
        case f64: {
          Ref ret;
          switch (curr->op) {
            case Neg:           ret = ValueBuilder::makeUnary(MINUS, value); break;
            case Abs:           ret = ValueBuilder::makeCall(MATH_ABS, value); break;
            case Ceil:          ret = ValueBuilder::makeCall(MATH_CEIL, value); break;
            case Floor:         ret = ValueBuilder::makeCall(MATH_FLOOR, value); break;
            case Trunc:         ret = ValueBuilder::makeCall(MATH_TRUNC, value); break;
            case Nearest:       ret = ValueBuilder::makeCall(MATH_NEAREST, value); break;
            case Sqrt:          ret = ValueBuilder::makeCall(MATH_SQRT, value); break;
            case TruncSFloat32: ret = ValueBuilder::makePrefix(B_NOT, ValueBuilder::makePrefix(B_NOT, value)); break;
            case PromoteFloat32:
            case ConvertSInt32: ret = ValueBuilder::makePrefix(PLUS, ValueBuilder::makeBinary(value, OR, ValueBuilder::makeNum(0))); break;
            case ConvertUInt32: ret = ValueBuilder::makePrefix(PLUS, ValueBuilder::makeBinary(value, TRSHIFT, ValueBuilder::makeNum(0))); break;
            case DemoteFloat64: ret = value; break;
            default: std::cerr << curr << '\n'; abort();
          }
          if (curr->type == f32) { // doubles need much less coercing
            return makeAsmCoercion(ret, ASM_FLOAT);
          }
          return ret;
        }
        default: abort();
      }
    }
    Ref visitBinary(Binary *curr) override {
      if (isStatement(curr)) {
        ScopedTemp tempLeft(curr->left->type, parent);
        GetLocal fakeLocalLeft;
        fakeLocalLeft.name = tempLeft.getName();
        ScopedTemp tempRight(curr->right->type, parent);
        GetLocal fakeLocalRight;
        fakeLocalRight.name = tempRight.getName();
        Binary fakeBinary = *curr;
        fakeBinary.left = &fakeLocalLeft;
        fakeBinary.right = &fakeLocalRight;
        Ref ret = blockify(visitAndAssign(curr->left, tempLeft));
        flattenAppend(ret, visitAndAssign(curr->right, tempRight));
        flattenAppend(ret, visitAndAssign(&fakeBinary, result));
        return ret;
      }
      // normal binary
      Ref left = visit(curr->left, EXPRESSION_RESULT);
      Ref right = visit(curr->right, EXPRESSION_RESULT);
      Ref ret;
      switch (curr->op) {
        case Add:      ret = ValueBuilder::makeBinary(left, PLUS, right); break;
        case Sub:      ret = ValueBuilder::makeBinary(left, MINUS, right); break;
        case Mul: {
          if (curr->type == i32) {
            return ValueBuilder::makeCall(MATH_IMUL, left, right); // TODO: when one operand is a small int, emit a multiply
          } else {
            return ValueBuilder::makeBinary(left, MINUS, right); break;
          }
        }
        case DivS:     ret = ValueBuilder::makeBinary(makeSigning(left, ASM_SIGNED),   DIV, makeSigning(right, ASM_SIGNED)); break;
        case DivU:     ret = ValueBuilder::makeBinary(makeSigning(left, ASM_UNSIGNED), DIV, makeSigning(right, ASM_UNSIGNED)); break;
        case RemS:     ret = ValueBuilder::makeBinary(makeSigning(left, ASM_SIGNED),   MOD, makeSigning(right, ASM_SIGNED)); break;
        case RemU:     ret = ValueBuilder::makeBinary(makeSigning(left, ASM_UNSIGNED), MOD, makeSigning(right, ASM_UNSIGNED)); break;
        case And:      ret = ValueBuilder::makeBinary(left, AND, right); break;
        case Or:       ret = ValueBuilder::makeBinary(left, OR, right); break;
        case Xor:      ret = ValueBuilder::makeBinary(left, XOR, right); break;
        case Shl:      ret = ValueBuilder::makeBinary(left, LSHIFT, right); break;
        case ShrU:     ret = ValueBuilder::makeBinary(left, TRSHIFT, right); break;
        case ShrS:     ret = ValueBuilder::makeBinary(left, RSHIFT, right); break;
        case Div:      ret = ValueBuilder::makeBinary(left, DIV, right); break;
        case Min:      ret = ValueBuilder::makeCall(MATH_MIN, left, right); break;
        case Max:      ret = ValueBuilder::makeCall(MATH_MAX, left, right); break;
        case Eq: {
          if (curr->left->type == i32) {
            return ValueBuilder::makeBinary(makeSigning(left, ASM_SIGNED), EQ, makeSigning(right, ASM_SIGNED));
          } else {
            return ValueBuilder::makeBinary(left, EQ, right);
          }
        }
        case Ne: {
          if (curr->left->type == i32) {
            return ValueBuilder::makeBinary(makeSigning(left, ASM_SIGNED), NE, makeSigning(right, ASM_SIGNED));
          } else {
            return ValueBuilder::makeBinary(left, NE, right);
          }
        }
        case LtS:      return ValueBuilder::makeBinary(makeSigning(left, ASM_SIGNED),   LT, makeSigning(right, ASM_SIGNED));
        case LtU:      return ValueBuilder::makeBinary(makeSigning(left, ASM_UNSIGNED), LT, makeSigning(right, ASM_UNSIGNED));
        case LeS:      return ValueBuilder::makeBinary(makeSigning(left, ASM_SIGNED),   LE, makeSigning(right, ASM_SIGNED));
        case LeU:      return ValueBuilder::makeBinary(makeSigning(left, ASM_UNSIGNED), LE, makeSigning(right, ASM_UNSIGNED));
        case GtS:      return ValueBuilder::makeBinary(makeSigning(left, ASM_SIGNED),   GT, makeSigning(right, ASM_SIGNED));
        case GtU:      return ValueBuilder::makeBinary(makeSigning(left, ASM_UNSIGNED), GT, makeSigning(right, ASM_UNSIGNED));
        case GeS:      return ValueBuilder::makeBinary(makeSigning(left, ASM_SIGNED),   GE, makeSigning(right, ASM_SIGNED));
        case GeU:      return ValueBuilder::makeBinary(makeSigning(left, ASM_UNSIGNED), GE, makeSigning(right, ASM_UNSIGNED));
        case Lt:       return ValueBuilder::makeBinary(left, LT, right);
        case Le:       return ValueBuilder::makeBinary(left, LE, right);
        case Gt:       return ValueBuilder::makeBinary(left, GT, right);
        case Ge:       return ValueBuilder::makeBinary(left, GE, right);
        default: abort();
      }
      return makeAsmCoercion(ret, wasmToAsmType(curr->type));
    }
    Ref visitSelect(Select *curr) override {
      if (isStatement(curr)) {
        ScopedTemp tempCondition(i32, parent);
        GetLocal fakeCondition;
        fakeCondition.name = tempCondition.getName();
        ScopedTemp tempIfTrue(curr->ifTrue->type, parent);
        GetLocal fakeLocalIfTrue;
        fakeLocalIfTrue.name = tempIfTrue.getName();
        ScopedTemp tempIfFalse(curr->ifFalse->type, parent);
        GetLocal fakeLocalIfFalse;
        fakeLocalIfFalse.name = tempIfFalse.getName();
        Select fakeSelect = *curr;
        fakeSelect.condition = &fakeCondition;
        fakeSelect.ifTrue = &fakeLocalIfTrue;
        fakeSelect.ifFalse = &fakeLocalIfFalse;
        Ref ret = blockify(visitAndAssign(curr->condition, tempCondition));
        flattenAppend(ret, visitAndAssign(curr->ifTrue, tempIfTrue));
        flattenAppend(ret, visitAndAssign(curr->ifFalse, tempIfFalse));
        flattenAppend(ret, visitAndAssign(&fakeSelect, result));
        return ret;
      }
      // normal select
      Ref condition = visit(curr->condition, EXPRESSION_RESULT);
      Ref ifTrue = visit(curr->ifTrue, EXPRESSION_RESULT);
      Ref ifFalse = visit(curr->ifFalse, EXPRESSION_RESULT);
      ScopedTemp tempCondition(i32, parent),
                 tempIfTrue(curr->type, parent),
                 tempIfFalse(curr->type, parent);
      return
        ValueBuilder::makeSeq(
          ValueBuilder::makeAssign(tempCondition.getAstName(), condition),
          ValueBuilder::makeSeq(
            ValueBuilder::makeAssign(tempIfTrue.getAstName(), ifTrue),
            ValueBuilder::makeSeq(
              ValueBuilder::makeAssign(tempIfFalse.getAstName(), ifFalse),
              ValueBuilder::makeConditional(tempCondition.getAstName(), tempIfTrue.getAstName(), tempIfFalse.getAstName())
            )
          )
        );
    }
    Ref visitHost(Host *curr) override {
      abort();
    }
    Ref visitNop(Nop *curr) override {
      return ValueBuilder::makeToplevel();
    }
    Ref visitUnreachable(Unreachable *curr) override {
      return ValueBuilder::makeCall(ABORT_FUNC);
    }
  };
  return ExpressionProcessor(this).visit(curr, result);
}

} // namespace wasm

