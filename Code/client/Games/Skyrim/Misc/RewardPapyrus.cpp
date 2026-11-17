#include <TiltedOnlinePCH.h>
#include <Misc/RewardPapyrus.h>
#include <Forms/TESQuest.h>
#include <Services/RewardService.h>
#include <charconv>
#include <cstring>
#include <optional>
#include <string_view>

namespace
{
std::optional<uint64_t> Hex(std::string_view text)
{
    if (text.size() != 16) return {};
    uint64_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? std::optional<uint64_t>{value} : std::nullopt;
}
std::string_view Text(BSScript::Variable* var)
{
    return var && var->type == BSScript::Variable::kString && var->data.s ? std::string_view(var->data.s) : std::string_view{};
}
std::optional<std::pair<uint64_t, uint64_t>> Ticket(std::string_view text)
{
    if (text.size() != 33 || text[16] != ':') return {};
    const auto epoch = Hex(text.substr(0, 16)), token = Hex(text.substr(17));
    if (!epoch || !*epoch || !token || !*token) return {};
    return std::make_pair(*epoch, *token);
}
struct RewardFunction final : BSScript::NativeFunction
{
    enum Operation { Register, Epoch, Mode, Submit, Complete, Claim, CanApply };
    Operation Op;
    uint32_t Count{};
    RewardFunction(BSScript::IVirtualMachine* vm, Operation op, const char* name, uint32_t count, BSScript::Variable::Type result)
        : NativeFunction(name, "RFABCoopRewards", true, count), Op(op), Count(count)
    {
        pFunction = nullptr; // no untyped function-pointer cast; the virtual dispatcher below owns every signature
        returnType = result;
        SetCallableFromTasklets(false);
        for (uint32_t i = 0; i < count; ++i)
        {
            reinterpret_cast<BSFixedString*>(&parameters.data[i].name)->Set("argument");
            parameters.data[i].type = BSScript::Variable::kString;
        }
        if (op == Register)
        {
            BSFixedString form("Form"); void* type{};
            vm->GetScriptObjectType1(&form, &type); parameters.data[0].pType = type;
            parameters.data[3].type = BSScript::Variable::kInteger;
            parameters.data[4].type = BSScript::Variable::kBoolean;
        }
        if (op == Submit) for (uint32_t i = 1; i <= 5; ++i) parameters.data[i].type = BSScript::Variable::kInteger;
        if (op == Complete) for (uint32_t i = 1; i <= 3; ++i) parameters.data[i].type = BSScript::Variable::kInteger;
    }
    bool HasCallback() override { return true; }
    bool MarshallAndDispatch(BSScript::Variable*, BSScript::IVirtualMachine* vm, uint32_t, BSScript::Variable* out, BSScript::StackFrame* frame) override
    {
        using Variable = BSScript::Variable;
        Variable* args[8]{};
        if (!out || Count > 8 || parameters.capacity != Count || (Count && (!parameters.data || !frame || !frame->pParent))) return false;
        const auto page = Count ? frame->GetPageForFrame() : 0;
        for (uint32_t i = 0; i < Count; ++i)
        {
            args[i] = frame->GetStackFrameVariable(i, page);
            if (!args[i]) return false;
            if (Op == Register && i == 0) continue; // Form has a complex VM type, not scalar kObject
            if (args[i]->type != parameters.data[i].type) return false;
        }
        switch (Op)
        {
        case Epoch:
        {
            const auto value = fmt::format("{:016x}", RewardBridgeClient::Epoch()); out->Set<const char*>(value.c_str()); return true;
        }
        case Mode: out->Set<int32_t>(static_cast<int32_t>(RewardBridgeClient::Mode())); return true;
        case Register:
        {
            auto* object = args[0]->GetObject();
            const auto signature = Hex(Text(args[2])), epoch = Hex(Text(args[5]));
            const auto name = Text(args[1]);
            Coop::Rewards::Adapter adapter;
            bool valid = object && signature && epoch && name.size() < sizeof(adapter.Name);
            if (valid)
            {
                adapter.Signature = *signature; adapter.Kinds = static_cast<uint32_t>(args[3]->data.i);
                adapter.Flags = Coop::Rewards::kRequiredFlags | (args[4]->data.b ? Coop::Rewards::Bizarre : 0u);
                adapter.Receiver = object->GetHandle(); std::copy(name.begin(), name.end(), std::begin(adapter.Name));
                auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
                valid = policy && policy->HandleIsType(static_cast<uint32_t>(TESQuest::Type), adapter.Receiver) &&
                    policy->IsHandleObjectAvailable(adapter.Receiver) && RewardBridgeClient::Register(*epoch, &adapter);
            }
            out->Set<bool>(valid); return true;
        }
        case Submit:
        {
            const auto epoch = Hex(Text(args[0])), life = Hex(Text(args[6])), occurrence = Hex(Text(args[7]));
            auto result = Coop::Rewards::Submission::Blocked;
            if (epoch && life && occurrence)
            {
                Coop::Rewards::Source source;
                source.WorldEpoch = *epoch; source.Kind = static_cast<uint32_t>(args[1]->data.i);
                source.SourceForm = static_cast<uint32_t>(args[2]->data.i); source.ContextForm = static_cast<uint32_t>(args[3]->data.i);
                source.ActorForm = static_cast<uint32_t>(args[4]->data.i); source.BaseXP = args[5]->data.i;
                source.Incarnation = *life; source.Occurrence = *occurrence;
                result = RewardBridgeClient::Submit(&source);
            }
            out->Set<int32_t>(static_cast<int32_t>(result)); return true;
        }
        case Complete:
        {
            const auto ticket = Ticket(Text(args[0]));
            bool valid = false;
            if (ticket)
            {
                const Coop::Rewards::Completion completion{ticket->first, ticket->second, static_cast<Coop::Rewards::Result>(args[1]->data.i),
                    args[2]->data.i, static_cast<uint32_t>(args[3]->data.i)};
                valid = RewardBridgeClient::Complete(&completion);
            }
            out->Set<bool>(valid); return true;
        }
        case Claim:
        case CanApply:
        {
            const auto ticket = Ticket(Text(args[0]));
            const bool valid = ticket && ticket->first == RewardBridgeClient::Epoch() && (Op == Claim ?
                RewardBridgeClient::Mailbox().ClaimApplication(ticket->first, ticket->second) :
                RewardBridgeClient::Mailbox().CanApply(ticket->first, ticket->second));
            out->Set<bool>(valid); return true;
        }
        }
        return false;
    }
};
}
void BindRFABRewardFunctions(BSScript::IVirtualMachine* vm)
{
    if (!vm) return;
    using V = BSScript::Variable;
    vm->BindNativeMethod(new RewardFunction(vm, RewardFunction::Register, "RegisterAdapter", 6, V::kBoolean));
    vm->BindNativeMethod(new RewardFunction(vm, RewardFunction::Epoch, "GetWorldEpoch", 0, V::kString));
    vm->BindNativeMethod(new RewardFunction(vm, RewardFunction::Mode, "GetMode", 0, V::kInteger));
    vm->BindNativeMethod(new RewardFunction(vm, RewardFunction::Submit, "Submit", 8, V::kInteger));
    vm->BindNativeMethod(new RewardFunction(vm, RewardFunction::Complete, "Complete", 4, V::kBoolean));
    vm->BindNativeMethod(new RewardFunction(vm, RewardFunction::Claim, "Claim", 1, V::kBoolean));
    vm->BindNativeMethod(new RewardFunction(vm, RewardFunction::CanApply, "CanApply", 1, V::kBoolean));
}
