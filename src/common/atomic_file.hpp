#pragma once
#include <string>

namespace atomic_file {

// content を path へアトミックに書き込む。
// 既存の path は削除せずに置換するため、失敗しても旧ファイルの内容は保持される。
// error が非 null の場合、失敗時に原因を表すメッセージを設定する。
bool write(const std::string& path, const std::string& content,
           std::string* error = nullptr);

} // namespace atomic_file
