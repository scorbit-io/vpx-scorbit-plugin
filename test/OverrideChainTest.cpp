// license:GPLv3+

// "Never a sibling" (docs/slice4-internals.md): which sources the overlay overrides, on the
// display lists seen in a live VPX. D is PinMAME's display, S32 and S64 Serum v2's two outputs,
// and ours are the plugin's own overrides, listed first because it subscribed first.

#include "OverrideChain.h"

#include "Check.h"

#include <string>
#include <vector>

using namespace Scorbit;
using Chain::Source;

namespace
{

constexpr uint64_t D = 10, S32 = 20, S64 = 21, UP = 30, OTHER = 40;
constexpr uint64_t OURS_D = 100, OURS_S32 = 101;

std::string Show(const std::vector<uint64_t>& ids)
{
   std::string s;
   for (const uint64_t id : ids)
      s += std::to_string(id) + " ";
   return s;
}

void TestUncolouredOverridesTheDisplay()
{
   CHECK(Chain::Targets({ { D, 0, false } }, D) == (std::vector<uint64_t> { D }));
   // Already overriding it changes nothing: ours is not a child that makes D a branch.
   const std::vector<uint64_t> t = Chain::Targets({ { OURS_D, D, true }, { D, 0, false } }, D);
   CHECK_MSG(t == (std::vector<uint64_t> { D }), Show(t));
}

void TestEarlyOrderOverridesEveryColouriserOutput()
{
   // Serum took the last match, PinMAME's own display, and both outputs override it.
   const std::vector<uint64_t> t = Chain::Targets(
      { { OURS_D, D, true }, { D, 0, false }, { S32, D, false }, { S64, D, false } }, D);
   CHECK_MSG(t == (std::vector<uint64_t> { S32, S64 }), "withdraw D, override both outputs: " + Show(t));
}

void TestLateOrderKeepsTheOverrideSerumReadsFrom()
{
   // Serum chained onto our override of D, so its outputs override ours.
   const std::vector<uint64_t> t = Chain::Targets(
      { { OURS_D, D, true }, { D, 0, false }, { S32, OURS_D, false }, { S64, OURS_D, false } }, D);
   CHECK_MSG(t == (std::vector<uint64_t> { D, S32, S64 }), "keep D, add both outputs: " + Show(t));
}

void TestOurOtherOverridesDoNotBranchTheChain()
{
   const std::vector<uint64_t> t = Chain::Targets(
      { { OURS_S32, S32, true }, { D, 0, false }, { S32, D, false } }, D);
   CHECK_MSG(t == (std::vector<uint64_t> { S32 }), Show(t));
}

void TestTheLeafOfALongerChain()
{
   // An upscaler on D, and a colouriser on the upscaler: only the end of the chain is a leaf.
   const std::vector<uint64_t> t = Chain::Targets({ { D, 0, false }, { UP, D, false }, { S32, UP, false } }, D);
   CHECK_MSG(t == (std::vector<uint64_t> { S32 }), Show(t));
}

void TestUnrelatedAndMissing()
{
   const std::vector<uint64_t> t = Chain::Targets({ { D, 0, false }, { OTHER, 0, false } }, D);
   CHECK_MSG(t == (std::vector<uint64_t> { D }), "a source that overrides nothing of ours is ignored: " + Show(t));
   CHECK_MSG(Chain::Targets({ { OTHER, 0, false } }, D).empty(), "no root, nothing to override");
   CHECK_MSG(Chain::Targets({ { OURS_D, D, true } }, D).empty(), "our own entry is not the root");
}

void TestACycleTerminates()
{
   // Never produced by a sane list, but the walk must end: D and S32 override each other.
   const std::vector<uint64_t> t = Chain::Targets({ { D, S32, false }, { S32, D, false } }, D);
   CHECK_MSG(t.empty(), "neither is a leaf: " + Show(t));
   const std::vector<uint64_t> self = Chain::Targets({ { D, D, false } }, D);
   CHECK_MSG(self == (std::vector<uint64_t> { D }), "a source naming itself is still a leaf: " + Show(self));
}

}

int main()
{
   TestUncolouredOverridesTheDisplay();
   TestEarlyOrderOverridesEveryColouriserOutput();
   TestLateOrderKeepsTheOverrideSerumReadsFrom();
   TestOurOtherOverridesDoNotBranchTheChain();
   TestTheLeafOfALongerChain();
   TestUnrelatedAndMissing();
   TestACycleTerminates();
   return ScorbitTest::Summary("override_chain_test");
}
