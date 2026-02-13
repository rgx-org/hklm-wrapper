#include "shim/registry_hooks_utils.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace hklmwrap;

namespace {

std::vector<uint8_t> AsBytes(const std::vector<char>& chars) {
  return std::vector<uint8_t>(chars.begin(), chars.end());
}

} // namespace

TEST_CASE("EnsureWideStringData trims REG_SZ bytes after first terminator", "[shim][registry]") {
  const auto input = AsBytes({'c', 'l', 'a', 'm', 'm', 'e', 't', '\0', 'X', 'Y'});

  const auto normalized = EnsureWideStringData(REG_SZ, input.data(), static_cast<DWORD>(input.size()));
  const auto ansi = WideToAnsiBytesForQuery(REG_SZ, normalized);

  const auto expected = AsBytes({'c', 'l', 'a', 'm', 'm', 'e', 't', '\0'});
  CHECK(ansi == expected);
}

TEST_CASE("EnsureWideStringData trims REG_EXPAND_SZ bytes after first terminator", "[shim][registry]") {
  const auto input = AsBytes({'%', 'T', 'M', 'P', '%', '\0', 'Z', 'Z'});

  const auto normalized = EnsureWideStringData(REG_EXPAND_SZ, input.data(), static_cast<DWORD>(input.size()));
  const auto ansi = WideToAnsiBytesForQuery(REG_EXPAND_SZ, normalized);

  const auto expected = AsBytes({'%', 'T', 'M', 'P', '%', '\0'});
  CHECK(ansi == expected);
}

TEST_CASE("EnsureWideStringData appends REG_SZ terminator when input is unterminated", "[shim][registry]") {
  const auto input = AsBytes({'a', 'b', 'c'});

  const auto normalized = EnsureWideStringData(REG_SZ, input.data(), static_cast<DWORD>(input.size()));
  const auto ansi = WideToAnsiBytesForQuery(REG_SZ, normalized);

  const auto expected = AsBytes({'a', 'b', 'c', '\0'});
  CHECK(ansi == expected);
}

TEST_CASE("EnsureWideStringData trims REG_MULTI_SZ at first double terminator", "[shim][registry]") {
  const auto input = AsBytes({'o', 'n', 'e', '\0', 't', 'w', 'o', '\0', '\0', 'J', 'U', 'N', 'K'});

  const auto normalized = EnsureWideStringData(REG_MULTI_SZ, input.data(), static_cast<DWORD>(input.size()));
  const auto ansi = WideToAnsiBytesForQuery(REG_MULTI_SZ, normalized);

  const auto expected = AsBytes({'o', 'n', 'e', '\0', 't', 'w', 'o', '\0', '\0'});
  CHECK(ansi == expected);
}

TEST_CASE("String conversion helpers pass through non-string binary data", "[shim][registry]") {
  const std::vector<uint8_t> input = {0xDE, 0xAD, 0x00, 0xBE, 0xEF};

  const auto normalized = EnsureWideStringData(REG_BINARY, input.data(), static_cast<DWORD>(input.size()));
  CHECK(normalized == input);

  const auto queried = WideToAnsiBytesForQuery(REG_BINARY, normalized);
  CHECK(queried == input);
}
