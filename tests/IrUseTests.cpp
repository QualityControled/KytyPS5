#include "graphics/shader/recompiler/ir/Value.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

// These CPU tests validate IR graph mutation. They never read guest memory or use a GPU.
namespace Common {
int DbgExitIfHandler(const char* expression, const char*, int) {
	throw std::runtime_error(std::string("IR invariant: ") + expression);
}
void DbgExit(int status) { std::exit(status); }
} // namespace Common

namespace {
namespace IR = Libs::Graphics::ShaderRecompiler::IR;
static_assert(sizeof(IR::Value) == 16u);
static_assert(sizeof(IR::Inst) == 112u);

void Check(bool condition, const char* reason) {
	if (!condition) throw std::runtime_error(reason);
}

struct EdgeHash {
	size_t operator()(const IR::Use& use) const {
		return std::hash<const IR::Inst*> {}(use.user) ^ (std::hash<size_t> {}(use.operand) << 1u);
	}
};

void CheckUses(const IR::Inst& producer) {
	std::unordered_set<IR::Use, EdgeHash> edges;
	for (const auto& use: producer.Uses()) {
		Check(use.user != nullptr && use.operand < use.user->NumArgs(), "reverse edge has no valid consumer operand");
		Check(use.user->Arg(use.operand).TryInstruction() == &producer, "reverse edge disagrees with consumer value");
		Check(edges.insert(use).second, "duplicate reverse edge for one consumer operand");
	}
	Check(edges.size() == producer.UseCount(), "reverse edge count disagrees with graph");
}

void TestInlineAndCopiedAliases() {
	IR::Inst first(IR::ValueOpcode::Identity);
	IR::Inst second(IR::ValueOpcode::Identity);
	first.SetArg(0u, IR::Value(uint32_t {7}));
	second.SetArg(0u, IR::Value(uint32_t {9}));
	IR::Inst consumer(IR::ValueOpcode::IAdd32);
	consumer.SetArg(0u, IR::Value(&first));
	consumer.SetArg(1u, consumer.Arg(0u));
	Check(first.UseCount() == 2u && consumer.Arg(0u) == IR::Value(&first) &&
	          consumer.Arg(1u).GetType() == IR::Type::U32,
	      "copied operand changed semantic identity, type or edge count");
	consumer.SetArg(0u, consumer.Arg(1u));
	CheckUses(first);
	consumer.SetArg(1u, IR::Value(&second));
	Check(first.UseCount() == 1u && second.UseCount() == 1u, "same-consumer reassignment lost an edge");
	CheckUses(first);
	CheckUses(second);
	consumer.SetArg(0u, consumer.Arg(1u));
	Check(first.UseCount() == 0u && second.UseCount() == 2u, "copied alias retained its previous owner edge");
	CheckUses(second);
	consumer.SetArg(1u, IR::Value(uint32_t {11}));
	Check(second.UseCount() == 1u && consumer.Arg(1u).U32() == 11u, "immediate substitution kept a reverse edge");
	consumer.Invalidate();
	Check(first.UseCount() == 0u && second.UseCount() == 0u, "inline invalidation did not unlink all operands");
}

void TestLargeOperandsAndPhiStorage() {
	IR::Inst first(IR::ValueOpcode::Identity);
	IR::Inst second(IR::ValueOpcode::Identity);
	first.SetArg(0u, IR::Value(uint32_t {7}));
	second.SetArg(0u, IR::Value(uint32_t {9}));
	IR::Inst large(IR::ValueOpcode::ExternalCallContextWord);
	Check(large.NumArgs() > 4u, "fixture no longer exercises out-of-line operand storage");
	for (size_t index = 0u; index < large.NumArgs(); ++index) large.SetArg(index, IR::Value(&first));
	large.SetArg(2u, large.Arg(4u));
	large.SetArg(3u, IR::Value(&second));
	Check(first.UseCount() == 4u && second.UseCount() == 1u, "out-of-line reassignment corrupted edge counts");
	IR::Inst phi(IR::ValueOpcode::Phi);
	phi.SetFlags(IR::Type::U32);
	for (size_t index = 0u; index < 64u; ++index) phi.AddPhiOperand(nullptr, IR::Value(&first));
	phi.SetArg(0u, IR::Value(&second));
	phi.SetArg(31u, phi.Arg(63u));
	phi.SetArg(63u, IR::Value(uint32_t {11}));
	Check(phi.NumArgs() == 64u && first.UseCount() == 66u && second.UseCount() == 2u,
	      "Phi growth or middle/end mutation lost reciprocal edges");
	CheckUses(first);
	CheckUses(second);
	first.ReplaceUsesWith(IR::Value(&second), false);
	Check(first.GetOpcode() == IR::ValueOpcode::Void && first.UseCount() == 0u && second.UseCount() == 68u,
	      "non-preserving replacement failed to redirect all inline/large/Phi consumers");
	CheckUses(second);
	phi.ReplaceUsesWith(IR::Value(uint32_t {13}), false);
	Check(second.UseCount() == 5u, "Phi invalidation left incoming reverse edges registered");
	large.Invalidate();
	Check(second.UseCount() == 0u, "large operand invalidation left producer edges");
}

void TestIdentityAndSelfReplacement() {
	IR::Inst replacement(IR::ValueOpcode::Identity);
	replacement.SetArg(0u, IR::Value(uint32_t {9}));
	IR::Inst producer(IR::ValueOpcode::Identity);
	producer.SetArg(0u, IR::Value(uint32_t {7}));
	IR::Inst observer(IR::ValueOpcode::IAdd32);
	observer.SetArg(0u, IR::Value(&producer));
	observer.SetArg(1u, IR::Value(&producer));
	producer.ReplaceUsesWith(observer.Arg(0u));
	producer.ReplaceUsesWith(IR::Value(&producer), false);
	Check(producer.GetOpcode() == IR::ValueOpcode::Identity && producer.Arg(0u).U32() == 7u &&
	          producer.UseCount() == 2u && observer.Arg(0u) == IR::Value(&producer),
	      "self replacement changed the definition or created an Identity self-cycle");
	producer.ReplaceUsesWith(IR::Value(&replacement));
	Check(producer.GetOpcode() == IR::ValueOpcode::Identity && producer.Arg(0u) == IR::Value(&replacement) &&
	          producer.UseCount() == 0u && replacement.UseCount() == 3u &&
	          observer.Arg(0u) == IR::Value(&replacement) && observer.Arg(1u).Resolve().U32() == 9u,
	      "preserving replacement lost identity, repeated consumer or resolved value");
	CheckUses(replacement);
	producer.Invalidate();
	Check(replacement.UseCount() == 2u, "retained Identity edge was not removed on invalidation");
	observer.Invalidate();
	Check(replacement.UseCount() == 0u, "replaced consumer cleanup retained stale edges");
}

void TestPhiCyclesAndDestruction() {
	IR::Inst a(IR::ValueOpcode::Phi);
	IR::Inst b(IR::ValueOpcode::Phi);
	a.SetFlags(IR::Type::U32);
	b.SetFlags(IR::Type::U32);
	a.AddPhiOperand(nullptr, IR::Value(&b));
	b.AddPhiOperand(nullptr, IR::Value(&a));
	a.AddPhiOperand(nullptr, IR::Value(&a));
	CheckUses(a);
	CheckUses(b);
	a.ReplaceUsesWith(IR::Value(uint32_t {0}), false);
	Check(a.UseCount() == 0u && b.UseCount() == 0u && b.Arg(0u).U32() == 0u,
	      "self or cyclic Phi substitution left stale reciprocal edges");
	b.Invalidate();
	IR::Inst producer(IR::ValueOpcode::Identity);
	producer.SetArg(0u, IR::Value(uint32_t {19}));
	{
		IR::Inst ephemeral(IR::ValueOpcode::IAdd32);
		ephemeral.SetArg(0u, IR::Value(&producer));
		ephemeral.SetArg(1u, IR::Value(&producer));
		Check(producer.UseCount() == 2u, "destruction fixture failed to attach edges");
	}
	Check(producer.UseCount() == 0u, "consumer destruction failed to unlink repeated operands");
}

void TestMutationModel() {
	std::array<std::unique_ptr<IR::Inst>, 8> producers;
	for (size_t index = 0u; index < producers.size(); ++index) {
		producers[index] = std::make_unique<IR::Inst>(IR::ValueOpcode::Identity);
		producers[index]->SetArg(0u, IR::Value(static_cast<uint32_t>(index)));
	}
	constexpr size_t count = 64u;
	constexpr size_t arity = 5u;
	std::array<std::unique_ptr<IR::Inst>, count> users;
	std::array<std::array<int, arity>, count> model;
	std::array<int, 8> producer_sources;
	producer_sources.fill(-1);
	for (size_t user = 0u; user < count; ++user) {
		users[user] = std::make_unique<IR::Inst>(IR::ValueOpcode::ExternalCallContextWord);
		for (size_t operand = 0u; operand < arity; ++operand) {
			model[user][operand] = static_cast<int>((user + operand) % producers.size());
			users[user]->SetArg(operand, IR::Value(producers[model[user][operand]].get()));
		}
	}
	const auto audit = [&] {
		std::array<size_t, 8> counts {};
		for (size_t user = 0u; user < count; ++user) {
			for (size_t operand = 0u; operand < arity; ++operand) {
				const auto source = model[user][operand];
				const auto value = users[user]->Arg(operand);
				if (source >= 0) {
					Check(value.TryInstruction() == producers[source].get(), "mutation model consumer source mismatch");
					++counts[source];
				} else {
					Check(value.IsImmediate() && value.U32() == 23u, "mutation model immediate bits changed");
				}
			}
		}
		for (size_t source = 0u; source < producers.size(); ++source) {
			if (producer_sources[source] >= 0) {
				Check(producers[source]->Arg(0u).TryInstruction() == producers[producer_sources[source]].get(),
				      "mutation model retained Identity source mismatch");
				++counts[producer_sources[source]];
			}
		}
		for (size_t source = 0u; source < producers.size(); ++source) {
			Check(producers[source]->UseCount() == counts[source], "mutation model reciprocal edge count mismatch");
			CheckUses(*producers[source]);
		}
	};
	uint32_t random = 0xa13743u;
	const auto next = [&] {
		random ^= random << 13u;
		random ^= random >> 17u;
		random ^= random << 5u;
		return random;
	};
	audit();
	for (size_t step = 0u; step < 20000u; ++step) {
		const auto user = next() % count;
		const auto operand = next() % arity;
		if (step % 3u == 0u) {
			const auto from_user = next() % count;
			const auto from_operand = next() % arity;
			model[user][operand] = model[from_user][from_operand];
			users[user]->SetArg(operand, users[from_user]->Arg(from_operand));
		} else {
			const auto source = static_cast<int>(next() % 10u) - 2;
			model[user][operand] = source;
			users[user]->SetArg(operand, source >= 0 ? IR::Value(producers[source].get()) : IR::Value(uint32_t {23}));
		}
		if (step == 2000u || step == 6000u || step == 10000u || step == 14000u) {
			const size_t source = (step - 2000u) / 4000u;
			const size_t replacement = source + 4u;
			for (auto& operands: model)
				for (auto& value: operands)
					if (value == static_cast<int>(source)) value = static_cast<int>(replacement);
			for (auto& value: producer_sources)
				if (value == static_cast<int>(source)) value = static_cast<int>(replacement);
			producer_sources[source] = static_cast<int>(replacement);
			producers[source]->ReplaceUsesWith(IR::Value(producers[replacement].get()));
		}
		if (step % 100u == 0u) audit();
	}
	audit();
	for (auto& user: users) user->Invalidate();
	for (auto& producer: producers) producer->Invalidate();
	for (const auto& producer: producers) Check(producer->UseCount() == 0u, "mutation model cleanup retained edges");
}

void TestHighFanout() {
	IR::Inst replacement(IR::ValueOpcode::Identity);
	replacement.SetArg(0u, IR::Value(uint32_t {9}));
	IR::Inst producer(IR::ValueOpcode::Identity);
	producer.SetArg(0u, IR::Value(uint32_t {7}));
	constexpr size_t count = 70000u; // Cross the U16 boundary; no timing assertion.
	std::vector<std::unique_ptr<IR::Inst>> users;
	users.reserve(count);
	for (size_t index = 0u; index < count; ++index) {
		auto user = std::make_unique<IR::Inst>(IR::ValueOpcode::Identity);
		user->SetArg(0u, IR::Value(&producer));
		users.push_back(std::move(user));
	}
	Check(producer.UseCount() == count, "high-fanout attachment truncated edge indexes");
	CheckUses(producer);
	for (size_t index: {size_t {0}, count / 2u, count - 1u}) users[index]->SetArg(0u, IR::Value(&replacement));
	Check(producer.UseCount() == count - 3u && replacement.UseCount() == 3u,
	      "high-fanout front/middle/end removal lost an edge");
	producer.ReplaceUsesWith(IR::Value(&replacement));
	Check(producer.UseCount() == 0u && replacement.UseCount() == count + 1u,
	      "high-fanout replacement truncated edges or omitted its retained Identity");
	for (const auto& user: users)
		Check(user->Arg(0u) == IR::Value(&replacement) && user->Arg(0u).Resolve().U32() == 9u,
		      "high-fanout replacement changed a consumer value");
	CheckUses(replacement);
	users.clear();
	Check(replacement.UseCount() == 1u, "high-fanout destruction retained consumer edges");
	producer.Invalidate();
	Check(replacement.UseCount() == 0u, "high-fanout Identity cleanup retained an edge");
}

} // namespace

int main() {
	try {
		TestInlineAndCopiedAliases();
		TestLargeOperandsAndPhiStorage();
		TestIdentityAndSelfReplacement();
		TestPhiCyclesAndDestruction();
		TestMutationModel();
		TestHighFanout();
		std::puts("IrUseTests: all six groups passed (CPU graph mutations; Value16/Inst112; no guest/GPU execution)");
		return 0;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "IrUseTests: failed: %s\n", error.what());
		return 1;
	}
}
