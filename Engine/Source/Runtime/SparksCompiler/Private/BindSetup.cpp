#include "CompilationChecker.h"
#include "LanguageNames.h"
#include "NameBinder.h"
#include "TypeChecker.h"

// 이름 해석의 공통 부분과 언어 이름·엔진 schema 선언·해석이다(Docs/Design/SparksLanguage
// 3.9절 "이름의 출처"·"타입 표기 해석", 10.2절).

namespace rocks::sparks::detail {

namespace {

constexpr StringView SchemaFix = "Regenerate the binding schema; do not edit it by hand.";

// 엔진 기반 체인을 따라갈 때의 상한이다(순환은 ResolveEngine이 끊지만 방어로 둔다).
constexpr uint32 MaxEngineChainSteps = 1u << 16;

bool SameKindFamily(SchemaTypeKind derived, SchemaTypeKind base) noexcept {
    return derived == base &&
           (derived == SchemaTypeKind::Object || derived == SchemaTypeKind::Asset ||
            derived == SchemaTypeKind::Interface || derived == SchemaTypeKind::Struct);
}

} // namespace

NameBinder::NameBinder(Compilation& compilation, DiagnosticList& diagnostics) noexcept
    : compilation_(compilation), diagnostics_(diagnostics), allocator_(*compilation.allocator_),
      names_(allocator_), foldedNames_(allocator_), moduleStates_(allocator_),
      packageNames_(allocator_), locals_(allocator_), scopeMarks_(allocator_),
      localIndex_(allocator_), importUsed_(allocator_), unresolvedNames_(allocator_) {
}

Result<void> NameBinder::Run() noexcept {
    Compilation& c = compilation_;
    if (auto pushed = c.types_.TryPushBack(TypeEntry{}); !pushed) {
        return pushed;
    }

    CollectModuleStates();
    if (error_ != ErrorCode::None) {
        return Result<void>::Failure(error_);
    }

    // 분석할 모듈의 노드 기록 구간을 잡는다. nodeBase_는 기록 배열을 다 잡은 뒤에 채운다(도중에
    // 실패해도 결과 접근이 범위 밖을 읽지 않게).
    const uint32 moduleCount = static_cast<uint32>(c.modules_.Size());
    usize total = 0;
    for (uint32 module = 0; module < moduleCount; ++module) {
        if (c.modules_[module].parsed && !moduleStates_[module].tooDeep) {
            total += c.trees_[module].NodeCount();
        }
    }

    if (auto resized = c.nodeSymbols_.TryResize(total); !resized) {
        return resized;
    }

    if (auto resized = c.nodeTypes_.TryResize(total); !resized) {
        return resized;
    }

    if (auto resized = c.exprTypes_.TryResize(total); !resized) {
        return resized;
    }

    for (usize index = 0; index < total; ++index) {
        c.nodeSymbols_[index] = NotRecorded;
        c.nodeTypes_[index] = NotRecorded;
        c.exprTypes_[index] = NotRecorded;
    }

    if (auto resized = c.nodeBase_.TryResize(moduleCount); !resized) {
        return resized;
    }

    total = 0;
    for (uint32 module = 0; module < moduleCount; ++module) {
        const bool bound = c.modules_[module].parsed && !moduleStates_[module].tooDeep;
        c.nodeBase_[module] = bound ? static_cast<uint32>(total) : NoNode;
        if (bound) {
            total += c.trees_[module].NodeCount();
        }
    }

    if (auto resized = importUsed_.TryResize(c.imports_.Size()); !resized) {
        return resized;
    }

    if (auto resized = unresolvedNames_.TryResize(moduleCount); !resized) {
        return resized;
    }

    for (uint8& used : importUsed_) {
        used = 0;
    }

    for (uint8& unresolved : unresolvedNames_) {
        unresolved = 0;
    }

    DeclareLanguageNames();
    DeclareEngine();
    ResolveEngine();
    DeclarePreludeMembers();
    for (uint32 module = 0; module < moduleCount; ++module) {
        if (c.nodeBase_[module] != NoNode) {
            DeclareModule(module);
        }
    }

    for (uint32 module = 0; module < moduleCount; ++module) {
        if (c.nodeBase_[module] != NoNode) {
            DeclareImports(module);
        }
    }

    // 기반과 상속을 먼저 정해야 멤버 타입 해석이 class의 불완전 여부를 안다.
    for (uint32 module = 0; module < moduleCount; ++module) {
        if (c.nodeBase_[module] != NoNode) {
            ResolveModuleBases(module);
        }
    }

    CheckInheritance();
    for (uint32 module = 0; module < moduleCount; ++module) {
        if (c.nodeBase_[module] != NoNode) {
            ResolveDeclarationTypes(module);
        }
    }

    for (uint32 module = 0; module < moduleCount; ++module) {
        if (c.nodeBase_[module] != NoNode) {
            CheckMembers(module);
            CheckInheritedNames(module);
        }
    }

    for (uint32 module = 0; module < moduleCount; ++module) {
        if (c.nodeBase_[module] != NoNode) {
            BindBodies(module);
        }
    }

    // 타입 검사(LC2c). 이름 해석의 결과와 이름 공간을 그대로 쓴다.
    if (error_ == ErrorCode::None && c.lastStage_ >= CompilationStage::Types) {
        TypeChecker checker(*this);
        checker.Run();
    }

    CheckServices();
    if (error_ != ErrorCode::None) {
        return Result<void>::Failure(error_);
    }

    return Result<void>::Success();
}

void NameBinder::Report(DiagnosticCode code, SourceId source, SourceSpan span, StringView message,
                        StringView explanation, StringView fix) noexcept {
    if (error_ != ErrorCode::None) {
        return;
    }

    if (auto added = diagnostics_.TryAdd(code, source, span, message, explanation, fix); !added) {
        error_ = added.Error();
    }
}

bool NameBinder::Succeeded(const StringBuilderBase& builder) noexcept {
    if (builder.HasFailed()) {
        error_ = builder.Status().Error();
        return false;
    }

    return error_ == ErrorCode::None;
}

uint32 NameBinder::AddSymbol(const Symbol& symbol) noexcept {
    if (error_ != ErrorCode::None) {
        return NoSymbol;
    }

    if (auto pushed = compilation_.symbols_.TryPushBack(symbol); !pushed) {
        error_ = pushed.Error();
        return NoSymbol;
    }

    return static_cast<uint32>(compilation_.symbols_.Size() - 1);
}

void NameBinder::InsertName(uint64 space, StringView name, uint32 symbol) noexcept {
    if (error_ != ErrorCode::None || symbol == NoSymbol || name.IsEmpty()) {
        return;
    }

    if (auto inserted = names_.Insert(NameKey{space, name}, symbol); !inserted) {
        error_ = inserted.Error();
        return;
    }

    if (auto inserted = foldedNames_.Insert(NameKey{space, name}, symbol); !inserted) {
        error_ = inserted.Error();
    }
}

uint32 NameBinder::FindName(uint64 space, StringView name) const noexcept {
    const uint32* found = names_.Find(NameKey{space, name});
    return found != nullptr ? *found : NoSymbol;
}

uint32 NameBinder::FindFolded(uint64 space, StringView name) const noexcept {
    const uint32* found = foldedNames_.Find(NameKey{space, name});
    return found != nullptr ? *found : NoSymbol;
}

uint32 NameBinder::InternType(uint32 symbol, Span<const uint32> arguments, bool optional) noexcept {
    if (symbol == NoSymbol || error_ != ErrorCode::None) {
        return NoType;
    }

    for (const uint32 argument : arguments) {
        if (argument == NoType) {
            return NoType; // 해석하지 못한 인자가 있으면 전체를 모른다
        }
    }

    Compilation& c = compilation_;
    // 같은 타입의 다른 이름(`int64`)은 정규 이름의 심볼로 바꾼다.
    const Symbol& entry = c.symbols_[symbol];
    if (entry.kind == SymbolKind::BuiltinType) {
        const StringView canonical = LanguageNames()[entry.index].canonical;
        if (!canonical.IsEmpty()) {
            symbol = FindName(SpaceKey(NameSpaceKind::Language, 0), canonical);
        }
    }

    if (const uint32* found =
            c.typeIndex_.Find(Compilation::TypeProbe{symbol, arguments, optional})) {
        return *found;
    }

    TypeEntry type;
    type.symbol = symbol;
    type.firstArgument = static_cast<uint32>(c.typeArguments_.Size());
    type.argumentCount = static_cast<uint32>(arguments.Size());
    type.optional = optional;
    if (auto appended = c.typeArguments_.TryAppend(arguments); !appended) {
        error_ = appended.Error();
        return NoType;
    }

    if (auto pushed = c.types_.TryPushBack(type); !pushed) {
        error_ = pushed.Error();
        return NoType;
    }

    const uint32 id = static_cast<uint32>(c.types_.Size() - 1);
    if (auto added = c.typeIndex_.Add(id); !added) {
        error_ = added.Error();
        return NoType;
    }

    return id;
}

uint32 NameBinder::AddSignature(uint32 returnType, Span<const uint32> parameters,
                                bool isAsync) noexcept {
    if (error_ != ErrorCode::None) {
        return NoSignature;
    }

    Compilation& c = compilation_;
    Signature signature;
    signature.returnType = returnType;
    signature.firstParameter = static_cast<uint32>(c.signatureParameters_.Size());
    signature.parameterCount = static_cast<uint32>(parameters.Size());
    signature.isAsync = isAsync;
    if (auto appended = c.signatureParameters_.TryAppend(parameters); !appended) {
        error_ = appended.Error();
        return NoSignature;
    }

    if (auto pushed = c.signatures_.TryPushBack(signature); !pushed) {
        error_ = pushed.Error();
        return NoSignature;
    }

    return static_cast<uint32>(c.signatures_.Size() - 1);
}

StringView NameBinder::NodeText(uint32 module, uint32 node) const noexcept {
    const SyntaxTree& tree = compilation_.trees_[module];
    const SyntaxNode& entry = tree.Node(node);
    if (entry.token == NoToken) {
        return StringView();
    }

    const SourceSpan span = tree.Tokens()[entry.token].span;
    const StringView text = compilation_.sources_->Get(compilation_.modules_[module].source).Text();
    return StringView(text.Data() + span.begin, span.end - span.begin);
}

SourceSpan NameBinder::NodeSpan(uint32 module, uint32 node) const noexcept {
    return compilation_.trees_[module].Node(node).span;
}

void NameBinder::SetNodeSymbol(uint32 module, uint32 node, uint32 symbol) noexcept {
    const uint32 base = compilation_.nodeBase_[module];
    if (base != NoNode && node != NoNode) {
        compilation_.nodeSymbols_[base + node] = symbol;
    }
}

void NameBinder::SetNodeType(uint32 module, uint32 node, uint32 type) noexcept {
    const uint32 base = compilation_.nodeBase_[module];
    if (base != NoNode && node != NoNode) {
        compilation_.nodeTypes_[base + node] = type;
    }
}

void NameBinder::DeclareLanguageNames() noexcept {
    const uint64 space = SpaceKey(NameSpaceKind::Language, 0);
    const Span<const LanguageName> names = LanguageNames();
    for (uint32 index = 0; index < names.Size(); ++index) {
        Symbol symbol;
        switch (names[index].kind) {
        case LanguageNameKind::BuiltinType:
            symbol.kind = SymbolKind::BuiltinType;
            break;
        case LanguageNameKind::PreludeType:
        case LanguageNameKind::TypeLiteral:
        case LanguageNameKind::ReservedType:
            symbol.kind = SymbolKind::PreludeType;
            break;
        case LanguageNameKind::PreludeValue:
        case LanguageNameKind::Literal:
            symbol.kind = SymbolKind::PreludeValue;
            break;
        }

        symbol.name = names[index].name;
        symbol.index = index;
        symbol.arity = names[index].arity;
        InsertName(space, symbol.name, AddSymbol(symbol));
    }

    for (const StringView name : ContextNames) {
        Symbol symbol;
        symbol.kind = SymbolKind::ContextName;
        symbol.name = name;
        const uint32 id = AddSymbol(symbol);
        if (firstContextSymbol_ == NoSymbol) {
            firstContextSymbol_ = id;
        }

        InsertName(space, name, id);
    }

    for (const StringView name : RpcPrefixes) {
        Symbol symbol;
        symbol.kind = SymbolKind::RpcPrefix;
        symbol.name = name;
        InsertName(space, name, AddSymbol(symbol));
    }

    voidType_ = InternType(FindName(space, "void"), Span<const uint32>(), false);
}

// 유효한 schema의 타입·멤버·타입 매개변수를 심볼로 만든다(타입 먼저 — 멤버의 owner가 된다).
void NameBinder::DeclareEngine() noexcept {
    Compilation& c = compilation_;
    const uint64 engine = SpaceKey(NameSpaceKind::Engine, 0);
    Array<uint32> firstType(allocator_);
    if (auto resized = firstType.TryResize(c.schemas_.Size()); !resized) {
        error_ = resized.Error();
        return;
    }

    for (uint32 schemaIndex = 0; schemaIndex < c.schemas_.Size(); ++schemaIndex) {
        firstType[schemaIndex] = static_cast<uint32>(c.symbols_.Size());
        if (!c.schemas_[schemaIndex].valid) {
            continue;
        }

        const BindingSchema& schema = c.schemas_[schemaIndex].schema;
        for (uint32 type = 0; type < schema.Types().Size(); ++type) {
            Symbol symbol;
            symbol.kind = SymbolKind::EngineType;
            symbol.name = schema.Text(schema.Types()[type].name);
            symbol.index = schemaIndex;
            symbol.item = type;
            InsertName(engine, symbol.name, AddSymbol(symbol));
        }
    }

    for (uint32 schemaIndex = 0; schemaIndex < c.schemas_.Size() && error_ == ErrorCode::None;
         ++schemaIndex) {
        if (!c.schemas_[schemaIndex].valid) {
            continue;
        }

        const BindingSchema& schema = c.schemas_[schemaIndex].schema;
        for (uint32 type = 0; type < schema.Types().Size(); ++type) {
            const SchemaType& entry = schema.Types()[type];
            const uint32 owner = firstType[schemaIndex] + type;
            const uint64 members = SpaceKey(NameSpaceKind::Members, owner);
            for (uint32 field = entry.firstField; field < entry.firstField + entry.fieldCount;
                 ++field) {
                Symbol symbol;
                symbol.kind = SymbolKind::EngineField;
                symbol.name = schema.Text(schema.Fields()[field].name);
                symbol.owner = owner;
                symbol.index = schemaIndex;
                symbol.item = field;
                InsertName(members, symbol.name, AddSymbol(symbol));
            }

            for (uint32 function = entry.firstFunction;
                 function < entry.firstFunction + entry.functionCount; ++function) {
                const SchemaFunction& source = schema.Functions()[function];
                Symbol symbol;
                symbol.kind = SymbolKind::EngineFunction;
                symbol.name = schema.Text(source.name);
                symbol.owner = owner;
                symbol.index = schemaIndex;
                symbol.item = function;
                symbol.arity = source.typeParameterCount;
                symbol.flags = (source.isStatic ? SymbolFlags::Static : 0) |
                               (source.isAsync ? SymbolFlags::Async : 0) |
                               (source.overridable ? SymbolFlags::Overridable : 0);
                const uint32 functionSymbol = AddSymbol(symbol);
                InsertName(members, symbol.name, functionSymbol);
                for (uint32 parameter = source.firstTypeParameter;
                     parameter < source.firstTypeParameter + source.typeParameterCount;
                     ++parameter) {
                    Symbol typeParameter;
                    typeParameter.kind = SymbolKind::EngineTypeParameter;
                    typeParameter.name = schema.Text(schema.TypeParameters()[parameter].name);
                    typeParameter.owner = functionSymbol;
                    typeParameter.index = schemaIndex;
                    typeParameter.item = parameter;
                    InsertName(SpaceKey(NameSpaceKind::Parameters, functionSymbol),
                               typeParameter.name, AddSymbol(typeParameter));
                }
            }

            for (uint32 event = entry.firstEvent; event < entry.firstEvent + entry.eventCount;
                 ++event) {
                Symbol symbol;
                symbol.kind = SymbolKind::EngineEvent;
                symbol.name = schema.Text(schema.Events()[event].name);
                symbol.owner = owner;
                symbol.index = schemaIndex;
                symbol.item = event;
                InsertName(members, symbol.name, AddSymbol(symbol));
            }

            for (uint32 item = entry.firstCase; item < entry.firstCase + entry.caseCount; ++item) {
                Symbol symbol;
                symbol.kind = SymbolKind::EnumCase;
                symbol.name = schema.Text(schema.Cases()[item].name);
                symbol.owner = owner;
                symbol.index = schemaIndex;
                symbol.item = item;
                InsertName(members, symbol.name, AddSymbol(symbol));
            }
        }
    }

    actor_ = FindName(engine, "Actor");
    component_ = FindName(engine, "Component");
    widgetController_ = FindName(engine, "WidgetController");
    worldService_ = FindName(engine, "WorldService");
}

void NameBinder::ReportSchema(uint32 schema, SourceSpan span, StringView message) noexcept {
    Report(DiagnosticCode::InvalidBindingSchema, compilation_.schemas_[schema].source, span,
           message, "", SchemaFix);
}

// schema의 `base`·`constraint`·타입 표기를 해석한다(3.9절 "타입 표기 해석"의 schema 부분).
void NameBinder::ResolveEngine() noexcept {
    Compilation& c = compilation_;
    const uint64 engine = SpaceKey(NameSpaceKind::Engine, 0);
    const uint32 symbolCount = static_cast<uint32>(c.symbols_.Size());
    // 기반.
    for (uint32 id = 0; id < symbolCount && error_ == ErrorCode::None; ++id) {
        Symbol& symbol = c.symbols_[id];
        if (symbol.kind != SymbolKind::EngineType) {
            continue;
        }

        const BindingSchema& schema = c.schemas_[symbol.index].schema;
        const SchemaType& type = schema.Types()[symbol.item];
        if (type.base.length == 0) {
            continue;
        }

        const StringView baseName = schema.Text(type.base);
        const uint32 base = FindName(engine, baseName);
        if (base == NoSymbol ||
            !SameKindFamily(
                type.kind,
                c.schemas_[c.symbols_[base].index].schema.Types()[c.symbols_[base].item].kind)) {
            StringBuilder<CompilationMessageInlineBytes> message(allocator_);
            message.AppendFormat("Base type '{}' is not a binding schema type of the same kind.",
                                 baseName);
            if (Succeeded(message)) {
                ReportSchema(symbol.index, type.baseSpan, message.View());
            }

            continue;
        }

        c.symbols_[id].base = base;
    }

    // 기반 순환: 순환에 든 타입 중 번호가 가장 작은 것의 `base`에 하나 내고 그 기반을 끊는다.
    Array<uint8> state(allocator_); // 0 미방문, 1 걷는 중, 2 끝
    Array<uint32> path(allocator_);
    if (auto resized = state.TryResize(symbolCount); !resized) {
        error_ = resized.Error();
        return;
    }

    for (uint32 id = 0; id < symbolCount; ++id) {
        state[id] = 0;
    }

    for (uint32 start = 0; start < symbolCount && error_ == ErrorCode::None; ++start) {
        if (c.symbols_[start].kind != SymbolKind::EngineType || state[start] != 0) {
            continue;
        }

        path.Clear();
        uint32 at = start;
        while (at != NoSymbol && state[at] == 0) {
            state[at] = 1;
            if (auto pushed = path.TryPushBack(at); !pushed) {
                error_ = pushed.Error();
                return;
            }

            at = c.symbols_[at].base;
        }

        if (at != NoSymbol && state[at] == 1) {
            uint32 smallest = at;
            for (uint32 member = c.symbols_[at].base; member != at;
                 member = c.symbols_[member].base) {
                smallest = member < smallest ? member : smallest;
            }

            const Symbol& symbol = c.symbols_[smallest];
            const SchemaType& type = c.schemas_[symbol.index].schema.Types()[symbol.item];
            ReportSchema(symbol.index, type.baseSpan, "The base types form a cycle.");
            c.symbols_[smallest].base = NoSymbol;
        }

        for (const uint32 walked : path) {
            state[walked] = 2;
        }
    }

    // 제약·타입 표기.
    for (uint32 id = 0; id < symbolCount && error_ == ErrorCode::None; ++id) {
        Symbol& symbol = c.symbols_[id];
        const uint32 schemaIndex = symbol.index;
        if (symbol.kind == SymbolKind::EngineTypeParameter) {
            const BindingSchema& schema = c.schemas_[schemaIndex].schema;
            const SchemaTypeParameter& parameter = schema.TypeParameters()[symbol.item];
            if (parameter.constraint.length == 0) {
                continue;
            }

            const StringView name = schema.Text(parameter.constraint);
            const uint32 constraint = FindName(engine, name);
            const SchemaTypeKind kind = constraint == NoSymbol
                                            ? SchemaTypeKind::Struct
                                            : c.schemas_[c.symbols_[constraint].index]
                                                  .schema.Types()[c.symbols_[constraint].item]
                                                  .kind;
            if (constraint == NoSymbol ||
                (kind != SchemaTypeKind::Object && kind != SchemaTypeKind::Interface)) {
                StringBuilder<CompilationMessageInlineBytes> message(allocator_);
                message.AppendFormat("Constraint '{}' is not an object or interface type of the "
                                     "binding schemas.",
                                     name);
                if (Succeeded(message)) {
                    ReportSchema(schemaIndex, parameter.constraintSpan, message.View());
                }

                continue;
            }

            c.symbols_[id].base = constraint;
        } else if (symbol.kind == SymbolKind::EngineField) {
            const BindingSchema& schema = c.schemas_[schemaIndex].schema;
            const SchemaField& field = schema.Fields()[symbol.item];
            const uint32 type =
                ResolveNotation(schemaIndex, NoSymbol, schema.Text(field.type), field.typeSpan);
            c.symbols_[id].type = type;
        } else if (symbol.kind == SymbolKind::EngineFunction ||
                   symbol.kind == SymbolKind::EngineEvent) {
            const BindingSchema& schema = c.schemas_[schemaIndex].schema;
            const bool isFunction = symbol.kind == SymbolKind::EngineFunction;
            uint32 first = 0;
            uint32 count = 0;
            uint32 returnType = voidType_;
            if (isFunction) {
                const SchemaFunction& function = schema.Functions()[symbol.item];
                first = function.firstParameter;
                count = function.parameterCount;
                if (function.returns.length != 0) {
                    returnType = ResolveNotation(schemaIndex, id, schema.Text(function.returns),
                                                 function.returnsSpan);
                }
            } else {
                const SchemaEvent& event = schema.Events()[symbol.item];
                first = event.firstParameter;
                count = event.parameterCount;
            }

            Array<uint32> parameters(allocator_);
            for (uint32 parameter = first; parameter < first + count; ++parameter) {
                const SchemaParameter& entry = schema.Parameters()[parameter];
                const uint32 type = ResolveNotation(schemaIndex, isFunction ? id : NoSymbol,
                                                    schema.Text(entry.type), entry.typeSpan);
                if (auto pushed = parameters.TryPushBack(type); !pushed) {
                    error_ = pushed.Error();
                    return;
                }
            }

            const uint32 signature = AddSignature(returnType, parameters.View(),
                                                  (symbol.flags & SymbolFlags::Async) != 0);
            c.symbols_[id].signature = signature;
        }
    }
}

uint32 NameBinder::ResolveNotation(uint32 schema, uint32 function, StringView text,
                                   SourceSpan span) noexcept {
    usize index = 0;
    bool failed = false;
    const uint32 type = ResolveNotationAt(schema, function, text, index, span, failed);
    return failed ? NoType : type;
}

// text[index]부터 표기 하나를 해석한다(문법은 reader가 이미 검사했다). 진단은 표기 하나에 하나다.
uint32 NameBinder::ResolveNotationAt(uint32 schema, uint32 function, StringView text, usize& index,
                                     SourceSpan span, bool& failed) noexcept {
    const usize begin = index;
    bool dotted = false;
    while (index < text.ByteCount() && text.Data()[index] != '<' && text.Data()[index] != '>' &&
           text.Data()[index] != ',' && text.Data()[index] != '?') {
        dotted = dotted || text.Data()[index] == '.';
        ++index;
    }

    const StringView name(text.Data() + begin, index - begin);
    Array<uint32> arguments(allocator_);
    if (index < text.ByteCount() && text.Data()[index] == '<') {
        ++index;
        while (error_ == ErrorCode::None) {
            const uint32 argument = ResolveNotationAt(schema, function, text, index, span, failed);
            if (auto pushed = arguments.TryPushBack(argument); !pushed) {
                error_ = pushed.Error();
                return NoType;
            }

            if (index < text.ByteCount() && text.Data()[index] == ',') {
                index += 2; // ", "
                continue;
            }

            ++index; // '>'
            break;
        }
    }

    const bool optional = index < text.ByteCount() && text.Data()[index] == '?';
    if (optional) {
        ++index;
    }

    if (failed) {
        return NoType; // 진단은 표기 하나에 하나
    }

    uint32 symbol = NoSymbol;
    if (!dotted && function != NoSymbol) {
        symbol = FindName(SpaceKey(NameSpaceKind::Parameters, function), name);
    }

    if (symbol == NoSymbol && !dotted) {
        symbol = FindName(SpaceKey(NameSpaceKind::Engine, 0), name);
    }

    if (symbol == NoSymbol && !dotted) {
        const uint32 language = FindName(SpaceKey(NameSpaceKind::Language, 0), name);
        if (language != NoSymbol && IsTypeSymbol(language)) {
            symbol = language;
        }
    }

    if (symbol == NoSymbol) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("Type '{}' is not known to the binding schemas.", name);
        if (Succeeded(message)) {
            ReportSchema(schema, span, message.View());
        }

        failed = true;
        return NoType;
    }

    if (compilation_.symbols_[symbol].arity != arguments.Size()) {
        StringBuilder<CompilationMessageInlineBytes> message(allocator_);
        message.AppendFormat("Type '{}' takes {} type arguments.", name,
                             compilation_.symbols_[symbol].arity);
        if (Succeeded(message)) {
            ReportSchema(schema, span, message.View());
        }

        failed = true;
        return NoType;
    }

    return InternType(symbol, arguments.View(), optional);
}

bool NameBinder::EngineChainHas(uint32 type, uint32 target) const noexcept {
    if (target == NoSymbol) {
        return false;
    }

    for (uint32 step = 0; type != NoSymbol && step < MaxEngineChainSteps; ++step) {
        if (type == target) {
            return true;
        }

        type = compilation_.symbols_[type].base;
    }

    return false;
}

bool NameBinder::IsTypeSymbol(uint32 symbol) const noexcept {
    if (symbol == NoSymbol) {
        return false;
    }

    const Symbol& entry = compilation_.symbols_[symbol];
    switch (entry.kind) {
    case SymbolKind::BuiltinType:
    case SymbolKind::EngineType:
    case SymbolKind::EngineTypeParameter:
    case SymbolKind::Class:
    case SymbolKind::Struct:
    case SymbolKind::Enum:
    case SymbolKind::Interface:
        return true;
    case SymbolKind::PreludeType:
        // 언어 이름 표 밖의 언어 타입(문맥 이름 Time의 타입)도 타입이다.
        return entry.index >= LanguageNames().Size() ||
               LanguageNames()[entry.index].kind != LanguageNameKind::ReservedType;
    case SymbolKind::LanguageTypeParameter:
        return true;
    default:
        return false;
    }
}

} // namespace rocks::sparks::detail
