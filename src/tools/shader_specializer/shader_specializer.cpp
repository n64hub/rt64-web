//
// RT64
//

// Prebuilt shaders for WebGPU (RT64_PREBUILT_SHADER_LIST in CMakeLists.txt), after spirv-opt has specialized each
// description and naga has written it as WGSL:
//
//   shader_specializer pack <list> <dir> <out.c> <header>
//       reads naga's <dir>/<description>.vs.wgsl, .ps.wgsl (dual-source blending) and .ps-single.wgsl (without)
//       for every line of the list, rewrites them as
//       PlumeWGSL.cmake does the other shaders, and writes them to <out.c> as one zstd frame and a table of where each
//       description's shaders are in it (rt64_prebuilt_shaders.h). Shaders that come out the same are kept once. A line
//       reading "section <name>" starts a section, a screen or level of the game, which it builds when it first draws
//       with its shaders; the lines above the first one are section 0, "boot", built before the game starts. A
//       description can be in several sections and is kept once. A line reading "background" starts one too.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <zstd.h>

static bool readFile(const std::string &path, std::string &data) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        fprintf(stderr, "shader_specializer: can't read %s\n", path.c_str());
        return false;
    }

    std::stringstream buffer;
    buffer << stream.rdbuf();
    data = buffer.str();
    return true;
}

static bool isIdentifierChar(char c) {
    return ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '_');
}

// naga writes each SPIR-V phi as a variable of its function, `var phi_<n>_: <type>;`, assigned on every edge into the
// block that reads it, and spirv-opt leaves a RasterPS hundreds of them in one function. Chrome parses a shader module
// (Tint) on its GPU process's main thread, the one that takes the page's presents, in a time that grows with a
// function's variables times its branches: a 180 KB RasterPS with ~300 of them took 100-120 ms, so each one built
// behind the game held the page's presents that long (Pilotwings 64's list, 45 such shaders, made 30 gaps of over 100
// ms in Chrome 155). As variables of the module (var<private>, one per invocation like a function's) the same shader
// parses in ~30 ms, and the list's 142 largest in 2.5 s against 6.7. A phi is always written before it is read, so
// it needn't start at zero each call. Tint reads a private variable as non-uniform where it tracked a function's, so
// the module turns the derivative uniformity check off, which naga's own output already passed.
static void privatizePhis(std::string &wgsl) {
    const std::string Removed(1, '\0');
    std::vector<std::string> lines;
    for (size_t start = 0; start <= wgsl.size();) {
        size_t end = wgsl.find('\n', start);
        if (end == std::string::npos) {
            end = wgsl.size();
        }

        lines.emplace_back(wgsl.substr(start, end - start));
        start = end + 1;
    }

    std::vector<size_t> functions;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].compare(0, 3, "fn ") == 0) {
            functions.emplace_back(i);
        }
    }

    std::string declarations;
    for (size_t f = 0; f < functions.size(); f++) {
        const size_t first = functions[f];
        const size_t last = (f + 1 < functions.size()) ? functions[f + 1] : lines.size();
        std::map<std::string, std::string> renamed;
        for (size_t i = first; i < last; i++) {
            std::string &line = lines[i];
            const size_t var = line.find_first_not_of(' ');
            if ((var == std::string::npos) || (var == 0) || (line.compare(var, 8, "var phi_") != 0) || (line.back() != ';') ||
                (line.find('=') != std::string::npos)) {
                continue;
            }

            const size_t colon = line.find(": ", var);
            if (colon == std::string::npos) {
                continue;
            }

            const std::string name = line.substr(var + 4, colon - var - 4);
            const std::string privateName = "phi_f" + std::to_string(f) + name.substr(3);
            if (wgsl.find(privateName) != std::string::npos) {
                return;
            }

            declarations += "var<private> " + privateName + line.substr(colon) + "\n";
            renamed[name] = privateName;
            line = Removed;
        }

        if (renamed.empty()) {
            continue;
        }

        for (size_t i = first; i < last; i++) {
            std::string &line = lines[i];
            if (line == Removed) {
                continue;
            }

            std::string rewritten;
            for (size_t p = 0; p < line.size();) {
                if (!isIdentifierChar(line[p])) {
                    rewritten.push_back(line[p++]);
                    continue;
                }

                size_t end = p;
                while ((end < line.size()) && isIdentifierChar(line[end])) {
                    end++;
                }

                const std::string token = line.substr(p, end - p);
                auto it = renamed.find(token);
                rewritten += (it != renamed.end()) ? it->second : token;
                p = end;
            }

            line = rewritten;
        }
    }

    if (declarations.empty()) {
        return;
    }

    // Before the first function and the attributes on the lines above it.
    size_t insertAt = functions[0];
    while ((insertAt > 0) && (lines[insertAt - 1].compare(0, 1, "@") == 0)) {
        insertAt--;
    }

    std::string result = "diagnostic(off, derivative_uniformity);\n";
    for (size_t i = 0; i < lines.size(); i++) {
        if (i == insertAt) {
            result += declarations;
        }

        if (lines[i] == Removed) {
            continue;
        }

        result += lines[i];
        if (i + 1 < lines.size()) {
            result.push_back('\n');
        }
    }

    wgsl = result;
}

// PlumeWGSL.cmake's rewrites: the push constant block becomes the uniform buffer plume's WebGPU backend gives it, and
// a storage texture is write-only unless its format can be read and written in core WebGPU. Then privatizePhis().
static void rewrite(std::string &wgsl) {
    for (const std::string block : { "var<immediate>", "var<push_constant>" }) {
        for (size_t p = wgsl.find(block); p != std::string::npos; p = wgsl.find(block, p)) {
            wgsl.replace(p, block.size(), "@group(3) @binding(999) var<uniform>");
        }
    }

    const std::string readWrite = ",read_write>";
    for (size_t p = wgsl.find(readWrite); p != std::string::npos; p = wgsl.find(readWrite, p + 1)) {
        const size_t open = wgsl.rfind('<', p);
        const std::string format = wgsl.substr(open + 1, p - open - 1);
        if ((format != "r32float") && (format != "r32uint") && (format != "r32sint")) {
            wgsl.replace(p, readWrite.size(), ",write>");
        }
    }

    privatizePhis(wgsl);
}

static int pack(char **argv) {
    std::string list;
    if (!readFile(argv[0], list)) {
        return 1;
    }

    const std::string dir = argv[1];
    std::string text;
    std::map<std::string, std::pair<uint32_t, uint32_t>> placed;
    std::stringstream table;
    uint32_t count = 0;
    std::map<std::string, uint32_t> indices;
    std::vector<std::string> sectionNames = { "boot" };
    std::vector<std::vector<uint32_t>> sections(1);
    std::istringstream lines(list);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream words(line);
        std::vector<std::string> description;
        std::string word;
        while (words >> word) {
            description.emplace_back(word);
        }

        if (!description.empty() && ((description[0] == "section") || (description[0] == "background"))) {
            sectionNames.emplace_back((description.size() > 1) ? description[1] : description[0]);
            sections.emplace_back();
            continue;
        }

        if ((description.size() != 5) || (description[0][0] == '#')) {
            continue;
        }

        std::string key = description[0];
        for (size_t i = 1; i < description.size(); i++) {
            key += "_" + description[i];
        }

        // A description already in the list joins this section too.
        auto indexIt = indices.find(key);
        if (indexIt != indices.end()) {
            std::vector<uint32_t> &section = sections.back();
            if (std::find(section.begin(), section.end(), indexIt->second) == section.end()) {
                section.emplace_back(indexIt->second);
            }

            continue;
        }

        indices[key] = count;
        sections.back().emplace_back(count);

        std::pair<uint32_t, uint32_t> where[3];
        const char *stages[3] = { ".vs.wgsl", ".ps.wgsl", ".ps-single.wgsl" };
        for (uint32_t s = 0; s < 3; s++) {
            std::string wgsl;
            if (!readFile(dir + "/" + key + stages[s], wgsl)) {
                return 1;
            }

            rewrite(wgsl);
            wgsl.push_back('\0');
            auto it = placed.find(wgsl);
            if (it == placed.end()) {
                it = placed.emplace(wgsl, std::make_pair(uint32_t(text.size()), uint32_t(wgsl.size()))).first;
                text += wgsl;
            }

            where[s] = it->second;
        }

        table << "    { 0x" << description[0] << "U, 0x" << description[1] << "U, 0x" << description[2] << "U, 0x" << description[3] << "U, 0x" << description[4] << "U, "
            << where[0].first << "U, " << where[0].second << "U, " << where[1].first << "U, " << where[1].second << "U, "
            << where[2].first << "U, " << where[2].second << "U },\n";
        count++;
    }

    // The shaders differ little from each other, so a window over all of them finds most of each one in the others.
    ZSTD_CCtx *context = ZSTD_createCCtx();
    ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, 12);
    ZSTD_CCtx_setParameter(context, ZSTD_c_windowLog, 26);
    ZSTD_CCtx_setParameter(context, ZSTD_c_enableLongDistanceMatching, 1);
    std::vector<uint8_t> compressed(ZSTD_compressBound(text.size()));
    const size_t compressedSize = ZSTD_compress2(context, compressed.data(), compressed.size(), text.data(), text.size());
    ZSTD_freeCCtx(context);
    if (ZSTD_isError(compressedSize)) {
        fprintf(stderr, "shader_specializer: zstd failed: %s\n", ZSTD_getErrorName(compressedSize));
        return 1;
    }

    std::ofstream out(argv[2], std::ios::binary);
    out << "// Generated by shader_specializer from " << argv[0] << ": " << count << " shaders, " << placed.size() << " different stages, "
        << text.size() << " bytes of WGSL in " << compressedSize << ".\n\n#include \"" << argv[3] << "\"\n\n";
    out << "const RT64PrebuiltShader RT64PrebuiltShaderList[] = {\n" << table.str() << "    { 0 }\n};\n\n";
    out << "const uint32_t RT64PrebuiltShaderCount = " << count << "U;\n\n";
    out << "const RT64PrebuiltShaderSection RT64PrebuiltShaderSections[] = {\n";
    uint32_t sectionFirst = 0;
    for (size_t s = 0; s < sections.size(); s++) {
        out << "    { \"" << sectionNames[s] << "\", " << sectionFirst << "U, " << sections[s].size() << "U },\n";
        sectionFirst += uint32_t(sections[s].size());
    }

    out << "};\n\n";
    out << "const uint32_t RT64PrebuiltShaderSectionCount = " << sections.size() << "U;\n";
    out << "const uint32_t RT64PrebuiltShaderSectionEntries[] = {";
    uint32_t entryCount = 0;
    for (const std::vector<uint32_t> &section : sections) {
        for (uint32_t index : section) {
            out << (((entryCount++ % 16) == 0) ? "\n    " : " ") << index << "U,";
        }
    }

    out << "\n    0U\n};\n\n";
    out << "const uint32_t RT64PrebuiltShaderTextSize = " << text.size() << "U;\n";
    out << "const uint32_t RT64PrebuiltShaderDataSize = " << compressedSize << "U;\n";
    out << "const unsigned char RT64PrebuiltShaderData[] = {";
    char hex[16];
    for (size_t i = 0; i < compressedSize; i++) {
        snprintf(hex, sizeof(hex), "%s%u,", ((i % 32) == 0) ? "\n    " : "", compressed[i]);
        out << hex;
    }

    out << "\n};\n";
    if (!out.good()) {
        return 1;
    }

    printf("shader_specializer: %u shaders in %zu sections, %zu different stages, %zu bytes of WGSL in %zu\n", count, sections.size(), placed.size(), text.size(), compressedSize);
    return 0;
}

int main(int argc, char *argv[]) {
    if ((argc == 6) && (std::string(argv[1]) == "pack")) {
        return pack(argv + 2);
    }

    fprintf(stderr, "usage: shader_specializer pack <list> <dir> <out.c> <header>\n");
    return 1;
}
