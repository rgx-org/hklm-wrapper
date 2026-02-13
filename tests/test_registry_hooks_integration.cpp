#include "common/local_registry_store.h"
#include "shim/registry_hooks_utils.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <array>
#include <filesystem>
#include <vector>

using namespace hklmwrap;

namespace {

std::wstring MakeTempDbPath() {
  auto base = std::filesystem::temp_directory_path() / "hklm-wrapper-tests";
  std::filesystem::create_directories(base);

  static size_t counter = 0;
  counter++;

  auto path = base / ("shim-int-" + std::to_string(counter) + ".sqlite");
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return path.wstring();
}

LONG QueryLikeRegQueryValueExA(const StoredValue& stored, BYTE* lpData, DWORD* lpcbData) {
  if (!lpcbData) {
    return ERROR_INVALID_PARAMETER;
  }

  const DWORD type = static_cast<DWORD>(stored.type);
  const std::vector<uint8_t> outBytes = WideToAnsiBytesForQuery(type, stored.data);
  const DWORD needed = static_cast<DWORD>(outBytes.size());

  if (!lpData) {
    *lpcbData = needed;
    return ERROR_SUCCESS;
  }
  if (*lpcbData < needed) {
    *lpcbData = needed;
    return ERROR_MORE_DATA;
  }

  if (needed > 0) {
    std::memcpy(lpData, outBytes.data(), needed);
  }
  *lpcbData = needed;
  return ERROR_SUCCESS;
}

std::vector<uint8_t> AsBytes(std::initializer_list<uint8_t> bytes) {
  return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

} // namespace

TEST_CASE("ANSI REG_SZ set->store->query probe flow does not leak trailing bytes", "[shim][integration]") {
  LocalRegistryStore store;
  REQUIRE(store.Open(MakeTempDbPath()));

  const std::wstring keyPath = L"HKLM\\Software\\RuneBreakers\\Ragnarok";
  const std::wstring valueName = L"ID";

  const auto setInput = AsBytes({'c', 'l', 'a', 'm', 'm', 'e', 't', '\0', 'x', 'x'});
  const auto normalized =
      EnsureWideStringData(REG_SZ, reinterpret_cast<const BYTE*>(setInput.data()), static_cast<DWORD>(setInput.size()));

  REQUIRE(store.PutValue(keyPath,
                         valueName,
                         REG_SZ,
                         normalized.empty() ? nullptr : normalized.data(),
                         static_cast<uint32_t>(normalized.size())));

  const auto stored = store.GetValue(keyPath, valueName);
  REQUIRE(stored.has_value());
  REQUIRE_FALSE(stored->isDeleted);
  REQUIRE(stored->type == REG_SZ);

  DWORD cb = 0;
  LONG rc = QueryLikeRegQueryValueExA(*stored, nullptr, &cb);
  CHECK(rc == ERROR_SUCCESS);
  CHECK(cb == 8);

  std::array<BYTE, 7> tooSmall{};
  cb = static_cast<DWORD>(tooSmall.size());
  rc = QueryLikeRegQueryValueExA(*stored, tooSmall.data(), &cb);
  CHECK(rc == ERROR_MORE_DATA);
  CHECK(cb == 8);

  std::array<BYTE, 8> exact{};
  cb = static_cast<DWORD>(exact.size());
  rc = QueryLikeRegQueryValueExA(*stored, exact.data(), &cb);
  CHECK(rc == ERROR_SUCCESS);
  CHECK(cb == 8);

  const auto expected = AsBytes({'c', 'l', 'a', 'm', 'm', 'e', 't', '\0'});
  CHECK(std::vector<uint8_t>(exact.begin(), exact.end()) == expected);
}

TEST_CASE("ANSI REG_MULTI_SZ set->store->query probe flow clamps at double terminator", "[shim][integration]") {
  LocalRegistryStore store;
  REQUIRE(store.Open(MakeTempDbPath()));

  const std::wstring keyPath = L"HKLM\\Software\\RuneBreakers\\Ragnarok";
  const std::wstring valueName = L"List";

  const auto setInput = AsBytes({'o', 'n', 'e', '\0', 't', 'w', 'o', '\0', '\0', 'x', 'x'});
  const auto normalized = EnsureWideStringData(
      REG_MULTI_SZ, reinterpret_cast<const BYTE*>(setInput.data()), static_cast<DWORD>(setInput.size()));

  REQUIRE(store.PutValue(keyPath,
                         valueName,
                         REG_MULTI_SZ,
                         normalized.empty() ? nullptr : normalized.data(),
                         static_cast<uint32_t>(normalized.size())));

  const auto stored = store.GetValue(keyPath, valueName);
  REQUIRE(stored.has_value());
  REQUIRE_FALSE(stored->isDeleted);
  REQUIRE(stored->type == REG_MULTI_SZ);

  DWORD cb = 0;
  LONG rc = QueryLikeRegQueryValueExA(*stored, nullptr, &cb);
  CHECK(rc == ERROR_SUCCESS);
  CHECK(cb == 9);

  std::array<BYTE, 8> tooSmall{};
  cb = static_cast<DWORD>(tooSmall.size());
  rc = QueryLikeRegQueryValueExA(*stored, tooSmall.data(), &cb);
  CHECK(rc == ERROR_MORE_DATA);
  CHECK(cb == 9);

  std::array<BYTE, 9> exact{};
  cb = static_cast<DWORD>(exact.size());
  rc = QueryLikeRegQueryValueExA(*stored, exact.data(), &cb);
  CHECK(rc == ERROR_SUCCESS);
  CHECK(cb == 9);

  const auto expected = AsBytes({'o', 'n', 'e', '\0', 't', 'w', 'o', '\0', '\0'});
  CHECK(std::vector<uint8_t>(exact.begin(), exact.end()) == expected);
}

TEST_CASE("ANSI REG_EXPAND_SZ set->store->query probe flow trims trailing bytes", "[shim][integration]") {
  LocalRegistryStore store;
  REQUIRE(store.Open(MakeTempDbPath()));

  const std::wstring keyPath = L"HKLM\\Software\\RuneBreakers\\Ragnarok";
  const std::wstring valueName = L"Path";

  const auto setInput = AsBytes({'%', 'T', 'E', 'M', 'P', '%', '\\', 'a', '\0', 'q', 'q'});
  const auto normalized = EnsureWideStringData(
      REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(setInput.data()), static_cast<DWORD>(setInput.size()));

  REQUIRE(store.PutValue(keyPath,
                         valueName,
                         REG_EXPAND_SZ,
                         normalized.empty() ? nullptr : normalized.data(),
                         static_cast<uint32_t>(normalized.size())));

  const auto stored = store.GetValue(keyPath, valueName);
  REQUIRE(stored.has_value());
  REQUIRE_FALSE(stored->isDeleted);
  REQUIRE(stored->type == REG_EXPAND_SZ);

  DWORD cb = 0;
  LONG rc = QueryLikeRegQueryValueExA(*stored, nullptr, &cb);
  CHECK(rc == ERROR_SUCCESS);
  CHECK(cb == 9);

  std::array<BYTE, 8> tooSmall{};
  cb = static_cast<DWORD>(tooSmall.size());
  rc = QueryLikeRegQueryValueExA(*stored, tooSmall.data(), &cb);
  CHECK(rc == ERROR_MORE_DATA);
  CHECK(cb == 9);

  std::array<BYTE, 9> exact{};
  cb = static_cast<DWORD>(exact.size());
  rc = QueryLikeRegQueryValueExA(*stored, exact.data(), &cb);
  CHECK(rc == ERROR_SUCCESS);
  CHECK(cb == 9);

  const auto expected = AsBytes({'%', 'T', 'E', 'M', 'P', '%', '\\', 'a', '\0'});
  CHECK(std::vector<uint8_t>(exact.begin(), exact.end()) == expected);
}

TEST_CASE("ANSI REG_SZ empty payload round-trips as single terminator", "[shim][integration]") {
  LocalRegistryStore store;
  REQUIRE(store.Open(MakeTempDbPath()));

  const std::wstring keyPath = L"HKLM\\Software\\RuneBreakers\\Ragnarok";
  const std::wstring valueName = L"EmptyText";

  const auto normalized = EnsureWideStringData(REG_SZ, nullptr, 0);
  REQUIRE(store.PutValue(keyPath,
                         valueName,
                         REG_SZ,
                         normalized.empty() ? nullptr : normalized.data(),
                         static_cast<uint32_t>(normalized.size())));

  const auto stored = store.GetValue(keyPath, valueName);
  REQUIRE(stored.has_value());
  REQUIRE_FALSE(stored->isDeleted);

  DWORD cb = 0;
  LONG rc = QueryLikeRegQueryValueExA(*stored, nullptr, &cb);
  CHECK(rc == ERROR_SUCCESS);
  CHECK(cb == 1);

  std::array<BYTE, 1> exact{};
  cb = static_cast<DWORD>(exact.size());
  rc = QueryLikeRegQueryValueExA(*stored, exact.data(), &cb);
  CHECK(rc == ERROR_SUCCESS);
  CHECK(cb == 1);
  CHECK(exact[0] == 0);
}

TEST_CASE("ANSI REG_MULTI_SZ empty payload round-trips as double terminator", "[shim][integration]") {
  LocalRegistryStore store;
  REQUIRE(store.Open(MakeTempDbPath()));

  const std::wstring keyPath = L"HKLM\\Software\\RuneBreakers\\Ragnarok";
  const std::wstring valueName = L"EmptyList";

  const auto normalized = EnsureWideStringData(REG_MULTI_SZ, nullptr, 0);
  REQUIRE(store.PutValue(keyPath,
                         valueName,
                         REG_MULTI_SZ,
                         normalized.empty() ? nullptr : normalized.data(),
                         static_cast<uint32_t>(normalized.size())));

  const auto stored = store.GetValue(keyPath, valueName);
  REQUIRE(stored.has_value());
  REQUIRE_FALSE(stored->isDeleted);

  DWORD cb = 0;
  LONG rc = QueryLikeRegQueryValueExA(*stored, nullptr, &cb);
  CHECK(rc == ERROR_SUCCESS);
  CHECK(cb == 2);

  std::array<BYTE, 2> exact{};
  cb = static_cast<DWORD>(exact.size());
  rc = QueryLikeRegQueryValueExA(*stored, exact.data(), &cb);
  CHECK(rc == ERROR_SUCCESS);
  CHECK(cb == 2);
  CHECK(exact[0] == 0);
  CHECK(exact[1] == 0);
}

TEST_CASE("ANSI query probe returns ERROR_INVALID_PARAMETER when lpcbData is null", "[shim][integration]") {
  LocalRegistryStore store;
  REQUIRE(store.Open(MakeTempDbPath()));

  const std::wstring keyPath = L"HKLM\\Software\\RuneBreakers\\Ragnarok";
  const std::wstring valueName = L"BadProbe";
  const auto setInput = AsBytes({'o', 'k', '\0'});
  const auto normalized =
      EnsureWideStringData(REG_SZ, reinterpret_cast<const BYTE*>(setInput.data()), static_cast<DWORD>(setInput.size()));

  REQUIRE(store.PutValue(keyPath,
                         valueName,
                         REG_SZ,
                         normalized.empty() ? nullptr : normalized.data(),
                         static_cast<uint32_t>(normalized.size())));

  const auto stored = store.GetValue(keyPath, valueName);
  REQUIRE(stored.has_value());
  REQUIRE_FALSE(stored->isDeleted);

  std::array<BYTE, 8> out{};
  const LONG rc = QueryLikeRegQueryValueExA(*stored, out.data(), nullptr);
  CHECK(rc == ERROR_INVALID_PARAMETER);
}
