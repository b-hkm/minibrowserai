#pragma once
#include <map>
#include <string>
#include <vector>

namespace browser {

enum class TokKind { TEXT, OPEN, CLOSE, SELFCLOSE, COMMENT, DOCTYPE, EOF_ };

struct Tok {
    TokKind kind = TokKind::EOF_;
    std::string name;
    std::string text;
    std::map<std::string, std::string> attrs;
};

std::vector<Tok> tokenize(const std::string& html);
std::map<std::string, std::string> parseAttrs(const std::string& inside);
std::string lower(std::string s);

} // namespace browser
