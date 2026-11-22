#include <SparksVm/Bytecode.h>

// bytecode 덤프(`.bc`, Docs/Design/SparksBytecode.ko.md 4.6절)다. 모듈 표만 읽으므로 컴파일 직후의
// 모듈과 `.sparkc`에서 읽은 모듈(LC4c)이 같은 글자를 낸다.

namespace rocks::sparks {

namespace {

char BankLetter(uint32 bank) noexcept {
    return bank == 0 ? 's' : bank == 1 ? 'r' : 'b';
}

char BankLetter(OperandBank bank) noexcept {
    return bank == OperandBank::Scalar ? 's' : bank == OperandBank::Ref ? 'r' : 'b';
}

class Writer final {
public:
    Writer(StringBuilderBase& out, const BytecodeModule& module) noexcept
        : out_(out), module_(module) {
    }

    void Write() noexcept {
        out_.Append("module ");
        if (module_.path != NoBytecodeIndex && module_.path < module_.stringOffsets.Size()) {
            Quoted(module_.String(module_.path));
        }

        out_.Append("\ncompiler ");
        if (module_.compiler != NoBytecodeIndex &&
            module_.compiler < module_.stringOffsets.Size()) {
            Quoted(module_.String(module_.compiler));
        }

        out_.AppendFormat("\nprofile {}\ncontent ", module_.profile == 0 ? "core" : "full");
        Hash(module_.contentHash);
        out_.Append("\ninterface ");
        Hash(module_.interfaceHash);
        out_.Append("\n");
        for (usize index = 0; index < module_.scalars.Size(); ++index) {
            out_.AppendFormat("k{} = {:#018x}\n", index, module_.scalars[index]);
        }

        for (usize index = 0; index < module_.stringOffsets.Size(); ++index) {
            out_.AppendFormat("t{} = ", index);
            Quoted(module_.String(static_cast<uint32>(index)));
            out_.Append("\n");
        }

        for (usize index = 0; index < module_.refs.Size(); ++index) {
            const BytecodeRef& ref = module_.refs[index];
            out_.AppendFormat("ref{} = {} {}", index, BytecodeRefKindName(ref.kind),
                              module_.String(ref.name));
            if (ref.kind == BytecodeRefKind::Literal && ref.detail != NoBytecodeIndex) {
                out_.Append(" ");
                Quoted(module_.String(ref.detail));
            } else if (ref.kind == BytecodeRefKind::Function && ref.detail != NoBytecodeIndex) {
                out_.AppendFormat(" #{}", ref.detail);
            } else if (ref.kind == BytecodeRefKind::Type && ref.detail != NoBytecodeIndex) {
                out_.AppendFormat(" type{}", ref.detail);
            } else if (ref.kind == BytecodeRefKind::StructField && ref.detail != NoBytecodeIndex) {
                out_.AppendFormat(" field{}", ref.detail);
            } else if (ref.detail != NoBytecodeIndex) {
                out_.AppendFormat(" type ref{}", ref.detail);
            }

            if (ref.arguments != NoBytecodeIndex) {
                out_.Append(" args ");
                Quoted(module_.String(ref.arguments));
            }

            out_.Append("\n");
        }

        // TYPE 표(형식 버전 2): struct는 bank별 slot 수와 필드(bank·slot 수·절대 위치), enum은
        // case.
        for (usize index = 0; index < module_.types.Size(); ++index) {
            const BytecodeType& type = module_.types[index];
            const bool structType = type.kind == BytecodeTypeKind::Struct;
            out_.AppendFormat("type{} = {} {}", index, structType ? "struct" : "enum",
                              module_.String(type.name));
            if (structType) {
                out_.AppendFormat(" s{} r{} b{}", type.slots[0], type.slots[1], type.slots[2]);
            }

            out_.Append("\n");
            for (uint32 at = type.firstField; at < type.firstField + type.fieldCount; ++at) {
                const BytecodeTypeField& field = module_.typeFields[at];
                out_.AppendFormat("  field{} {}", at, module_.String(field.name));
                if (structType) {
                    out_.AppendFormat(" {}{} x{}", BankLetter(field.bank), field.offset,
                                      field.slots);
                }

                out_.Append("\n");
            }
        }

        // CLASS 표(형식 버전 3): 기반, 자기 필드 bank별 slot 수, 초기화 함수,
        // 필드·hook·handler·이벤트.
        for (usize index = 0; index < module_.classes.Size(); ++index) {
            const BytecodeClass& entry = module_.classes[index];
            out_.AppendFormat("class{} = {}", index, module_.String(entry.name));
            if (entry.baseClass != NoBytecodeIndex) {
                out_.AppendFormat(" : class{}", entry.baseClass);
            } else if (entry.baseName != NoBytecodeIndex) {
                out_.AppendFormat(" : {}", module_.String(entry.baseName));
            }

            out_.AppendFormat(" s{} r{} b{}", entry.slots[0], entry.slots[1], entry.slots[2]);
            if (entry.initializer != NoBytecodeIndex) {
                out_.AppendFormat(" init {}",
                                  module_.String(module_.functions[entry.initializer].name));
            }

            out_.Append("\n");
            for (uint32 at = entry.firstField; at < entry.firstField + entry.fieldCount; ++at) {
                const BytecodeTypeField& field = module_.classFields[at];
                out_.AppendFormat("  field {} {}{} x{}{}\n", module_.String(field.name),
                                  BankLetter(field.bank), field.offset, field.slots,
                                  (field.flags & BytecodeFieldHeld) != 0 ? " held" : "");
            }

            for (uint32 at = entry.firstHook; at < entry.firstHook + entry.hookCount; ++at) {
                const BytecodeHook& hook = module_.hooks[at];
                out_.AppendFormat("  hook {} {}\n", BytecodeHookKindName(hook.kind),
                                  module_.String(module_.functions[hook.function].name));
            }

            for (uint32 at = entry.firstHandler; at < entry.firstHandler + entry.handlerCount;
                 ++at) {
                const BytecodeHandler& handler = module_.handlers[at];
                const StringView path = module_.String(handler.path);
                out_.AppendFormat("  handler {}{}{} {}{}{}", path, path.IsEmpty() ? "" : ".",
                                  module_.String(handler.event),
                                  module_.String(module_.functions[handler.function].name),
                                  handler.awaits != 0 ? " await" : "",
                                  handler.reentry == BytecodeReentry::SkipWhileBusy
                                      ? " skipWhileBusy"
                                  : handler.reentry == BytecodeReentry::Restart ? " restart"
                                                                                : "");
                // 경로 참조(ref 번호)와 이벤트 참조.
                for (uint32 step = handler.firstPath; step < handler.firstPath + handler.pathCount;
                     ++step) {
                    out_.AppendFormat(" ref{}", module_.handlerPaths[step]);
                }

                if (handler.eventRef != NoBytecodeIndex) {
                    out_.AppendFormat(" -> ref{}", handler.eventRef);
                }

                out_.Append("\n");
            }

            for (uint32 at = entry.firstEvent; at < entry.firstEvent + entry.eventCount; ++at) {
                const BytecodeEvent& event = module_.events[at];
                out_.AppendFormat("  event {}", module_.String(event.name));
                if (event.arguments != NoBytecodeIndex) {
                    out_.AppendFormat(" args \"{}\"", module_.String(event.arguments));
                }

                out_.Append("\n");
            }
        }

        for (const BytecodeFunction& function : module_.functions) {
            Function(function);
        }
    }

private:
    void Quoted(StringView text) noexcept {
        out_.Append("\"");
        for (usize index = 0; index < text.ByteCount(); ++index) {
            const char c = text.Data()[index];
            if (c == '"' || c == '\\') {
                out_.AppendFormat("\\{}", c);
            } else if (c == '\n') {
                out_.Append("\\n");
            } else if (c == '\t') {
                out_.Append("\\t");
            } else if (static_cast<uint8>(c) < 0x20 || static_cast<uint8>(c) >= 0x7F) {
                // 제어 문자와 ASCII 밖 byte는 `\xHH`다(덤프는 ASCII — 읽은 파일의 글자가 올바른
                // UTF-8이 아니어도 덤프한다).
                out_.AppendFormat("\\x{:02x}", static_cast<uint32>(static_cast<uint8>(c)));
            } else {
                out_.AppendFormat("{}", c);
            }
        }

        out_.Append("\"");
    }

    void Hash(const uint8 (&bytes)[BytecodeHashBytes]) noexcept {
        for (const uint8 byte : bytes) {
            out_.AppendFormat("{:02x}", static_cast<uint32>(byte));
        }
    }

    void Register(OperandBank bank, uint32 number) noexcept {
        out_.AppendFormat("{}{}", BankLetter(bank), number);
    }

    void Function(const BytecodeFunction& function) noexcept {
        out_.Append("\nfunction ");
        out_.Append(module_.String(function.name));
        out_.AppendFormat("  scalar={}/{} ref={}/{} buf={}/{} params={}/{}/{} returns {}x{}\n",
                          function.registers[0], function.frame[0], function.registers[1],
                          function.frame[1], function.registers[2], function.frame[2],
                          function.parameters[0], function.parameters[1], function.parameters[2],
                          BankLetter(static_cast<uint32>(function.returnBank)),
                          function.returnSlots);
        out_.Append("  body ");
        Hash(function.bodyHash);
        out_.Append("\n");
        if (function.heldCount != 0) {
            out_.AppendFormat("  held r{} x{}\n", function.heldFirst, function.heldCount);
        }
        for (uint32 index = function.firstAwait; index < function.firstAwait + function.awaitCount;
             ++index) {
            const BytecodeAwaitPoint& point = module_.awaits[index];
            out_.AppendFormat("  await {:04}  live", point.address);
            for (uint32 at = point.firstLive; at < point.firstLive + point.liveCount; ++at) {
                const BytecodeLiveRange& range = module_.liveRanges[at];
                const char letter = BankLetter(static_cast<uint32>(range.bank));
                out_.AppendFormat(" {}{}", letter, range.first);
                if (range.count > 1) {
                    out_.AppendFormat("..{}{}", letter, range.first + range.count - 1);
                }
            }

            out_.Append("\n");
        }

        for (uint32 index = function.firstSwitch;
             index < function.firstSwitch + function.switchCount; ++index) {
            const BytecodeSwitchTable& table = module_.switches[index];
            out_.AppendFormat("  switch{} [", index);
            for (uint32 at = table.firstCase; at < table.firstCase + table.caseCount; ++at) {
                out_.AppendFormat("{}{}: {:04}", at == table.firstCase ? "" : ", ",
                                  module_.cases[at].value, module_.cases[at].target);
            }

            out_.AppendFormat("] else {:04}\n", table.defaultTarget);
        }

        for (uint32 address = 0; address < function.wordCount; ++address) {
            const uint32 word = module_.words[function.firstWord + address];
            const OpcodeInfo& info = InfoOf(DecodeOpcode(word));
            const bool hasExtra = info.extra && address + 1 < function.wordCount;
            const uint32 extra = hasExtra ? module_.words[function.firstWord + address + 1] : 0;
            out_.AppendFormat("  {:04}  {}", address, info.name);
            Operands(info, word, extra, address);
            const BytecodeLocation& at = module_.locations[function.firstWord + address];
            out_.AppendFormat("    ; {}:{}\n", at.line, at.column);
            address += hasExtra ? 1 : 0;
        }
    }

    void Ref(uint32 index) noexcept {
        out_.AppendFormat("ref{}", index);
    }

    void Marker(uint32 marker) noexcept {
        out_.AppendFormat(" [{}x{} key {}x{}]", BankLetter((marker >> 5) & 0x3), marker & 0x1F,
                          BankLetter((marker >> 13) & 0x3), (marker >> 8) & 0x1F);
    }

    void Operands(const OpcodeInfo& info, uint32 word, uint32 extra, uint32 address) noexcept {
        const uint32 a = DecodeA(word);
        const uint32 b = DecodeB(word);
        const uint32 c = DecodeC(word);
        out_.Append("  ");
        switch (info.op) {
        case Opcode::LoadI:
            Register(info.a, a);
            out_.AppendFormat(", {}", DecodeSBx(word));
            return;
        case Opcode::LoadK:
            Register(info.a, a);
            out_.AppendFormat(", k{}", DecodeBx(word));
            return;
        case Opcode::LoadStr:
            Register(info.a, a);
            out_.AppendFormat(", t{}", DecodeBx(word));
            return;
        case Opcode::Jmp:
            out_.AppendFormat("{:04}", static_cast<int32>(address + 1) + DecodeSBx(word));
            return;
        case Opcode::Brt:
        case Opcode::Brf:
            Register(info.a, a);
            out_.AppendFormat(", {:04}", static_cast<int32>(address + 1) + DecodeSBx(word));
            return;
        case Opcode::Switch:
            Register(info.a, a);
            out_.AppendFormat(", switch{}", DecodeBx(word));
            return;
        case Opcode::Ret:
            if (b == 0) {
                out_.Append("-");
            } else {
                out_.AppendFormat("{}{} x{}", BankLetter(c), a, b);
            }

            return;
        case Opcode::RetErrS:
        case Opcode::EndExecS:
            out_.AppendFormat("s{} x{}", a, b);
            return;
        case Opcode::Conv:
            Register(info.a, a);
            out_.Append(", ");
            Register(info.b, b);
            out_.AppendFormat(", {} -> {}", ScalarKindName(static_cast<ScalarKind>(c >> 4)),
                              ScalarKindName(static_cast<ScalarKind>(c & 0xF)));
            return;
        case Opcode::GetCtx:
        case Opcode::NewS:
        case Opcode::NewC:
        case Opcode::NewA:
        case Opcode::NewSet:
        case Opcode::NewM:
            Register(info.a, a);
            out_.Append(", ");
            Ref(DecodeBx(word));
            return;
        case Opcode::Call:
        case Opcode::CallW:
        case Opcode::CallV:
        case Opcode::CallN:
        case Opcode::CallL:
        case Opcode::CallRpc:
        case Opcode::Emit:
            Ref(DecodeBx(word));
            return;
        case Opcode::WaitEv:
            Register(OperandBank::Ref, a);
            out_.Append(", ");
            Register(OperandBank::Ref, b);
            out_.Append(", ");
            Ref(DecodeAx(extra));
            return;
        case Opcode::Concat:
            Register(info.a, a);
            out_.AppendFormat(", b{} x{}", b, c);
            return;
        case Opcode::Fault:
            out_.AppendFormat("{}", DecodeAx(word));
            return;
        case Opcode::Extra:
            out_.AppendFormat("{}", DecodeAx(word));
            return;
        default:
            break;
        }

        // 일반 모양: 레지스터 피연산자(bank를 모르는 자리는 EXTRA 표지가 정한다)와 EXTRA.
        const uint32 marker = DecodeAx(extra);
        const OperandBank valueBank = static_cast<OperandBank>(((marker >> 5) & 0x3) + 1);
        const OperandBank keyBank = static_cast<OperandBank>(((marker >> 13) & 0x3) + 1);
        switch (info.op) {
        case Opcode::GetE:
            Register(valueBank, a);
            out_.Append(", ");
            Register(OperandBank::Buf, b);
            out_.Append(", ");
            Register(keyBank, c);
            Marker(marker);
            return;
        case Opcode::SetE:
            Register(OperandBank::Buf, a);
            out_.Append(", ");
            Register(keyBank, b);
            out_.Append(", ");
            Register(valueBank, c);
            Marker(marker);
            return;
        case Opcode::GetKey:
        case Opcode::GetVal:
            Register(valueBank, a);
            out_.Append(", ");
            Register(OperandBank::Buf, b);
            out_.Append(", ");
            Register(OperandBank::Scalar, c);
            Marker(marker);
            return;
        case Opcode::Append:
            Register(OperandBank::Buf, a);
            out_.Append(", ");
            Register(valueBank, b);
            Marker(marker);
            return;
        case Opcode::Put:
            Register(OperandBank::Buf, a);
            out_.Append(", ");
            Register(keyBank, b);
            out_.Append(", ");
            Register(valueBank, c);
            Marker(marker);
            return;
        case Opcode::Await: {
            const uint32 slots = marker & 0xFF;
            if (slots == 0) {
                out_.Append("-");
            } else {
                Register(static_cast<OperandBank>((marker >> 8) + 1), a);
                out_.AppendFormat(" x{}", slots);
            }

            out_.Append(", ");
            Register(OperandBank::Ref, b);
            return;
        }
        case Opcode::TryS:
        case Opcode::TryB:
            if (info.op == Opcode::TryS) {
                out_.AppendFormat("s{}, ", a);
            } else if ((c & 0x1F) == 0) {
                out_.Append("-, ");
            } else {
                out_.AppendFormat("{}{} x{}, ", BankLetter(c >> 5), a, c & 0x1F);
            }

            Register(info.b, b);
            if (info.op == Opcode::TryS) {
                out_.AppendFormat(" x{}", c);
            }

            out_.AppendFormat(", else {:04}", static_cast<int32>(address + 2) + DecodeSAx(extra));
            return;
        case Opcode::Ok:
            Register(info.a, a);
            out_.Append(", ");
            Register(static_cast<OperandBank>((marker >> 8) + 1), b);
            out_.AppendFormat(" x{}", marker & 0xFF);
            return;
        case Opcode::NewLit:
            out_.AppendFormat("{}{}, ", BankLetter(marker >> 22), a);
            Ref(marker & MaxBx);
            out_.AppendFormat(" args {}", (marker >> 16) & 0x3F);
            return;
        case Opcode::ToStr:
            Register(info.a, a);
            out_.Append(", ");
            Register(info.b, b);
            out_.AppendFormat(", display {}", marker >> 16);
            if ((marker >> 16) == static_cast<uint32>(ScalarKind::Enum)) {
                out_.Append(" ");
                Ref(marker & MaxBx);
            }

            return;
        case Opcode::EqN:
        case Opcode::NeN:
        case Opcode::EqNF:
        case Opcode::NeNF:
            Register(info.a, a);
            out_.Append(", ");
            Register(info.b, b);
            out_.Append(", ");
            Register(info.c, c);
            out_.AppendFormat(" x{}", marker);
            return;
        default:
            break;
        }

        // A·B·C 레지스터(bank가 정해진 자리)와 참조 EXTRA.
        const OperandBank banks[] = {info.a, info.b, info.c};
        const uint32 values[] = {a, b, c};
        const usize count = info.form == OperandForm::A ? 1 : info.form == OperandForm::AB ? 2 : 3;
        for (usize index = 0; index < count; ++index) {
            if (banks[index] == OperandBank::None) {
                continue;
            }

            out_.Append(index == 0 ? "" : ", ");
            Register(banks[index], values[index]);
        }

        if (info.extra) {
            out_.Append(", ");
            Ref(marker);
        }
    }

    StringBuilderBase& out_;
    const BytecodeModule& module_;
};

} // namespace

Result<void> AppendBytecodeDump(StringBuilderBase& out, const BytecodeModule& module) noexcept {
    Writer(out, module).Write();
    return out.Status();
}

} // namespace rocks::sparks
