#pragma once
#include <string>

namespace atomic_file {

// content を path へアトミックに書き込む。
// 同ディレクトリの "<path>.tmp" に書き出した後、既存ファイルを削除せずに
// 置換する (Windows: MoveFileExW + MOVEFILE_REPLACE_EXISTING、それ以外: rename)。
// 失敗した場合は既存の path の内容を保持したまま false を返し、
// "<path>.tmp" は残さない。
bool write(const std::string& path, const std::string& content);

} // namespace atomic_file
