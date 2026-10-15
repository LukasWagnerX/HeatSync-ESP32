#pragma once
#include <stddef.h>
#include <stdint.h>

static inline bool heatSyncPreferDuePoint(size_t candidate, uint32_t lastReadAt, uint32_t now, uint32_t minimumAge,
    const size_t *chosen, size_t chosenCount, size_t &best, uint32_t &bestAge) {
  for (size_t i = 0; i < chosenCount; ++i) if (chosen[i] == candidate) return false;
  const uint32_t age = lastReadAt ? (uint32_t) (now - lastReadAt) : UINT32_MAX;
  if (age < minimumAge || (best != SIZE_MAX && age <= bestAge)) return false;
  best = candidate; bestAge = age;
  return true;
}

static inline size_t heatSyncNextCatalogPoint(size_t &cursor, size_t count, const size_t *chosen, size_t chosenCount) {
  if (!count) return SIZE_MAX;
  for (size_t attempt = 0; attempt < count; ++attempt) {
    const size_t candidate = cursor % count;
    cursor = (candidate + 1) % count;
    bool duplicate = false;
    for (size_t index = 0; index < chosenCount; ++index) if (chosen[index] == candidate) duplicate = true;
    if (!duplicate) return candidate;
  }
  return SIZE_MAX;
}
