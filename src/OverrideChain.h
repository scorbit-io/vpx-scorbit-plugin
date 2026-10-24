// license:GPLv3+

#pragma once

// Which display sources the overlay overrides (docs/slice4-internals.md, "never a sibling").
//
// Starting at the controller's display, the override chain is walked through every other
// plugin's sources. Each leaf gets the plugin's override, so ours is never beside another
// override of the same source, which would let a link started at the controller stop at ours
// and lose a colouriser's output. A source that overrides one of ours is taken to sit on what
// ours overrides, and that source keeps our override: the colouriser chained onto us and reads
// from it. Free of any Visual Pinball type so the rule is tested on its own.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace Scorbit::Chain
{

struct Source
{
   uint64_t id = 0;
   uint64_t overrides = 0;  // 0 when it overrides nothing
   bool ours = false;
};

// The ids to override, in walk order. Empty when the root is not listed.
inline std::vector<uint64_t> Targets(const std::vector<Source>& sources, uint64_t root)
{
   if (std::none_of(sources.begin(), sources.end(), [root](const Source& s) { return !s.ours && s.id == root; }))
      return { };

   std::vector<uint64_t> kept;
   std::vector<uint64_t> parents(sources.size(), 0);
   for (size_t i = 0; i < sources.size(); i++)
   {
      if (sources[i].ours)
         continue;
      uint64_t parent = sources[i].overrides;
      const auto ours = std::find_if(sources.begin(), sources.end(), [parent](const Source& s) { return s.ours && s.id == parent; });
      if (ours != sources.end())
      {
         parent = ours->overrides;
         kept.push_back(parent);
      }
      parents[i] = parent;
   }

   std::vector<uint64_t> out;
   std::vector<uint64_t> queue { root };
   for (size_t q = 0; q < queue.size(); q++)
   {
      const uint64_t node = queue[q];
      bool leaf = true;
      for (size_t i = 0; i < sources.size(); i++)
      {
         if (sources[i].ours || parents[i] != node || sources[i].id == node)
            continue;
         leaf = false;
         if (std::find(queue.begin(), queue.end(), sources[i].id) == queue.end())
            queue.push_back(sources[i].id);
      }
      if (leaf || std::find(kept.begin(), kept.end(), node) != kept.end())
         out.push_back(node);
   }
   return out;
}

}
