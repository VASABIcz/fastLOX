#include <string>
#include <string_view>
#include <fstream>
#include <vector>
#include <sstream>
#include "utils/stringify.h"

struct Label {
    size_t offset;
    std::string text;
};

std::vector<Label> parseFile(std::istream& idk) {
    long items;
    idk >> items;

    std::vector<Label> res;

    for (auto i = 0; i < items; i++) {
        std::string text;
        long idex;
        idk >> idex;
        std::getline(idk, text);
        std::getline(idk, text);
        res.emplace_back(idex, text);
    }

    return res;
}

struct ObjdumpLine {
    size_t offset;
    std::string line;
};

long parseB16(std::string_view idk) {
    size_t acu = 0;

    for (auto i = 0UL; i < idk.size(); i++) {
        auto c = idk[i];
        if (c >= 'a' && c <= 'f') {
            acu += (idk[i]-'a')+10;
        } else {
            acu += idk[i]-'0';
        }
        if (i != idk.size()-1) acu *= 16;
    }

    return acu;
}

std::vector<ObjdumpLine> parseObjdump(std::istream& stream) {
    std::vector<ObjdumpLine> res;

    while (not stream.eof()) {
        std::string line;
        std::getline(stream, line);

        std::stringstream lineStream(line);

        std::string token;
        lineStream >> token;

        if (not token.ends_with(':')) continue;

        auto offset = parseB16({token.data(), token.size()-1});

        res.emplace_back(offset, line);
    }

    res.erase(res.begin());

    return res;
}

#define OKGREEN "\033[92m"
#define WARNING "\033[93m"
#define FAIL "\033[91m"
#define ENDC "\033[0m"

int main() {
    auto hintName = "fib";
    auto bytesFile = hintName;

    std::ifstream hintStream{stringify("hints/{}", hintName)};
    auto hints = parseFile(hintStream);

    system(stringify("unbuffer objdump --wide -bbinary -mi386:x86-64:intel -D bins/{} > .hint-tmp", bytesFile).c_str());

    std::ifstream objDumpFile{stringify(".hint-tmp")};
    auto bytes = parseObjdump(objDumpFile);

    size_t hintsIndex = 0;

    for (auto& line : bytes) {
        while (hintsIndex < hints.size()) {
            auto hint = hints[hintsIndex];
            if (hint.offset <= line.offset) {
                std::cout << OKGREEN << "[hint] " << ENDC << hint.text << std::endl;
                hintsIndex += 1;
                continue;
            }
            break;
        }
        auto idex = line.line.find("QWORD PTR");
        if (idex != line.line.npos) line.line.erase(idex, sizeof("QWORD PTR"));
        std::cout << line.line << std::endl;
    }
}