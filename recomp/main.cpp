#include "analysis.hpp"
#include "codegen.hpp"
#include "eerecomp_gen.hpp"
#include "elf.hpp"
#include "snr2.hpp"
#include "xff2.hpp"

#include "ps2/os.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

using namespace recomp;

namespace {

std::size_t code_size(const Program &program) {
    std::size_t code = 0;
    for (std::uint32_t pc = program.base; pc < program.end; pc += 4) code += program.is_code(pc);
    return code;
}

std::uint32_t word_at(const Snr2Module &m, std::uint32_t at) {
    std::uint32_t value;
    std::memcpy(&value, m.bytes.data() + at, 4);
    return value;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>> checks_for(const Snr2Module &m, const Program &program) {
    std::vector<std::uint32_t> words;
    for (std::uint32_t pc = m.code_begin; pc + 4 <= m.code_end; pc += 4)
        if (program.is_code(pc) && !m.links.count(pc) && word_at(m, pc)) words.push_back(pc);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> checks{{0, m.magic}};
    for (std::size_t k = 0; k < 8 && !words.empty(); ++k) {
        const std::uint32_t pc = words[k * words.size() / 8];
        if (checks.back().first != pc) checks.emplace_back(pc, word_at(m, pc));
    }
    return checks;
}

std::vector<Snr2Module> read_modules(const std::string &dir) {
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::directory_iterator(ps2::os::path(dir)))
        if (entry.is_regular_file()) paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    std::vector<Snr2Module> modules;
    std::map<std::string, int> names;
    for (const auto &path : paths) {
        std::ifstream file(path, std::ios::binary);
        std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
        auto module = read_snr2_module(bytes);
        if (!module) module = read_xff2_module(std::move(bytes), ps2::os::utf8(path.filename()));
        if (!module) continue;
        if (std::any_of(modules.begin(), modules.end(), [&](const Snr2Module &m) { return m.bytes == module->bytes; })) continue;
        if (const int n = names[module->name]++) module->name += "_" + std::to_string(n + 1);
        modules.push_back(std::move(*module));
    }
    return modules;
}

// A code window the game rewrites at runtime is a segment of the executable
// with no file contents. The pipeline captures what the game unpacks into it
// as files named <base>_<name>.bin, one content per file, and this reads them
// all as images of the one window, each a module to translate for that base.
std::vector<Snr2Module> read_overlays(const std::string &dir) {
    std::vector<Snr2Module> overlays;
    for (const auto &path : std::filesystem::directory_iterator(ps2::os::path(dir))) {
        if (!path.is_regular_file() || path.path().extension() != ".bin") continue;
        const std::string stem = ps2::os::utf8(path.path().stem());
        const auto split = stem.find('_');
        if (split == std::string::npos) continue;
        const std::string base = stem.substr(0, split);
        if (base.empty() || base.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) continue;
        Snr2Module o;
        std::ifstream file(path.path(), std::ios::binary);
        o.bytes = std::vector<std::uint8_t>{std::istreambuf_iterator<char>(file), {}};
        if (o.bytes.size() < 0x400u || o.bytes.size() & 3u) continue;
        o.size = static_cast<std::uint32_t>(o.bytes.size());
        o.base = static_cast<std::uint32_t>(std::stoul(base, nullptr, 16));
        o.code_begin = 0;
        o.code_end = o.size;
        o.name = stem;
        if (std::any_of(overlays.begin(), overlays.end(), [&](const Snr2Module &m) { return m.base == o.base && m.bytes == o.bytes; })) continue;
        overlays.push_back(std::move(o));
    }
    std::sort(overlays.begin(), overlays.end(), [](const Snr2Module &a, const Snr2Module &b) { return a.name < b.name; });
    return overlays;
}

// One switched instruction per line: ADDRESS WORD COND_ADDRESS VALUE, all hex; '#' starts a comment.
std::map<std::uint32_t, Toggle> read_toggles(const std::string &path) {
    std::ifstream in(ps2::os::path(path));
    if (!in) throw std::runtime_error("cannot read " + path);
    std::map<std::uint32_t, Toggle> toggles;
    for (std::string text; std::getline(in, text);) {
        text = text.substr(0, text.find('#'));
        std::istringstream fields(text);
        std::uint32_t address, word, cond, value;
        if (!(fields >> std::hex >> address >> word >> cond >> value)) continue;
        toggles[address] = {word, cond, static_cast<std::uint16_t>(value)};
    }
    return toggles;
}

int usage() {
    std::fprintf(stderr, "usage: eerecomp <elf> [--entries file] [--modules dir] [--overlays dir] [--probes file] [--toggles file] [--out dir] [--stats]\n");
    std::fprintf(stderr, "       eerecomp --live-image <captured.bin> --base <hex> --stamp <hex> --out <dir> [--entry <hex>]... [--expect-name <base_crc>] [--dense-checks]\n");
    return 2;
}

}

int main(int argc, char **argv) {
#ifdef _WIN32
    // argv is in the ANSI code page and loses every character outside it: take the UTF-16 command line and use it as UTF-8
    std::vector<std::string> utf8_args;
    std::vector<char *> utf8_argv;
    int wide_argc = 0;
    if (wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &wide_argc)) {
        for (int i = 0; i < wide_argc; ++i) utf8_args.push_back(ps2::os::utf8(wide[i]));
        LocalFree(wide);
        for (std::string &a : utf8_args) utf8_argv.push_back(a.data());
        utf8_argv.push_back(nullptr);
        argc = wide_argc;
        argv = utf8_argv.data();
    }
#endif
    if (argc < 2) return usage();
    if (std::strcmp(argv[1], "--live-image") == 0) {
        if (argc < 3) return usage();
        std::string image_path = argv[2], out_dir, expected_name;
        std::uint32_t base = 0;
        std::uint64_t stamp = 0;
        bool have_base = false, have_stamp = false, dense_checks = false;
        std::vector<std::uint32_t> entries;
        try {
            for (int i = 3; i < argc; ++i) {
                const std::string arg = argv[i];
                if (arg == "--base" && i + 1 < argc) {
                    const unsigned long long value = std::stoull(argv[++i], nullptr, 16);
                    if (value > UINT32_MAX) return usage();
                    base = static_cast<std::uint32_t>(value);
                    have_base = true;
                } else if (arg == "--stamp" && i + 1 < argc) {
                    stamp = std::stoull(argv[++i], nullptr, 16);
                    have_stamp = true;
                } else if (arg == "--out" && i + 1 < argc) out_dir = argv[++i];
                else if (arg == "--expect-name" && i + 1 < argc) expected_name = argv[++i];
                else if (arg == "--entry" && i + 1 < argc) {
                    const unsigned long long value = std::stoull(argv[++i], nullptr, 16);
                    if (value > UINT32_MAX) return usage();
                    entries.push_back(static_cast<std::uint32_t>(value));
                } else if (arg == "--dense-checks") dense_checks = true;
                else return usage();
            }
            if (image_path.empty() || out_dir.empty() || !have_base || !have_stamp || !stamp) return usage();
            std::ifstream input(ps2::os::path(image_path), std::ios::binary);
            if (!input) throw std::runtime_error("cannot open captured image");
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (bytes.size() > UINT32_MAX - base) throw std::runtime_error("image exceeds 32-bit address space");
            const std::string name = window_name(base, bytes.data(), bytes.size());
            if (!expected_name.empty() && expected_name != name) throw std::runtime_error("image content does not match expected window name");
            std::filesystem::create_directories(ps2::os::path(out_dir));
            LiveRequest request;
            request.base = base;
            request.image = std::move(bytes);
            request.name = name;
            request.prefix = "m_" + name + "_offline_";
            request.entries = std::move(entries);
            request.stamp = stamp;
            request.sparse_checks = !dense_checks;
            const LiveResult result = generate_live(request, ps2::os::path(out_dir));
            std::printf("live-image %s base=0x%08X bytes=%zu sparse=%d ok=%d code=%zu units=%zu accepted=%zu rejected=%zu\n",
                        name.c_str(), base, request.image.size(), request.sparse_checks, result.ok, result.code,
                        result.units, result.accepted.size(), result.rejected.size());
            for (const std::uint32_t pc : result.rejected) std::printf("rejected 0x%08X\n", pc);
            if (!result.ok) std::fprintf(stderr, "live-image error: %s\n", result.error.c_str());
            return result.ok ? 0 : 1;
        } catch (const std::exception &error) {
            std::fprintf(stderr, "live-image error: %s\n", error.what());
            return 1;
        }
    }
    std::string elf_path = argv[1], entries_path, out, probes_path, toggles_path, modules_dir, overlays_dir;
    bool stats = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--entries" && i + 1 < argc) entries_path = argv[++i];
        else if (arg == "--stats") stats = true;
        else if (arg == "--out" && i + 1 < argc) out = argv[++i];
        else if (arg == "--probes" && i + 1 < argc) probes_path = argv[++i];
        else if (arg == "--toggles" && i + 1 < argc) toggles_path = argv[++i];
        else if (arg == "--modules" && i + 1 < argc) modules_dir = argv[++i];
        else if (arg == "--overlays" && i + 1 < argc) overlays_dir = argv[++i];
        else return usage();
    }

    try {
        const Image image = Image::load(elf_path);
        const Entries entries = entries_path.empty() ? Entries{} : read_entries(entries_path);
        const Links imports = snr2_imports(image);
        Program program = analyse(image, entries.main, dynamic_sites(imports));
        std::map<Op, std::size_t> histogram;
        for (std::uint32_t pc = program.base; pc < program.end; pc += 4)
            if (program.is_code(pc)) ++histogram[decode(*image.word(pc), pc).op];
        const std::size_t code = code_size(program);
        std::printf("code %zu instructions (%zu KiB), functions %zu, entries %zu, rejected seeds %zu\n",
                    code, code * 4 / 1024, program.functions.size(), program.entries.size(), program.rejected.size());
        if (stats)
            for (const auto &[op, count] : histogram) std::printf("%10zu %s\n", count, name(op));

        std::vector<Translation> translations{{&image, &program, imports.empty() ? nullptr : &imports, "", false, "", 0, {}}};
        const std::vector<Snr2Module> modules = modules_dir.empty() ? std::vector<Snr2Module>{} : read_modules(modules_dir);
        std::deque<Image> images;
        std::deque<Program> programs;
        std::size_t module_code = 0;
        // Module code that calls the boot program at an address the linker
        // resolved (a jump without a relocation): entry points of the program.
        std::set<std::uint32_t> called_out;
        for (const Snr2Module &m : modules) {
            for (const std::uint32_t t : m.program_entries)
                if (t >= program.base && t < program.end && !program.entries.count(t)) called_out.insert(t);
            for (const std::uint32_t site : dynamic_sites(m.links)) {
                if (site < m.code_begin || site + 4 > m.code_end) continue;
                const std::uint32_t t = (word_at(m, site) & 0x03FFFFFFu) << 2;
                if (m.links.at(site).value == kAbsolute && t >= program.base && t < program.end && !program.entries.count(t)) called_out.insert(t);
            }
            images.push_back(snr2_image(m));
            std::vector<std::uint32_t> seeds = m.seeds;
            if (const auto it = entries.modules.find(m.name); it != entries.modules.end()) seeds.insert(seeds.end(), it->second.begin(), it->second.end());
            programs.push_back(analyse(images.back(), seeds, dynamic_sites(m.links)));
            module_code += code_size(programs.back());
            translations.push_back({&images.back(), &programs.back(), &m.links, "m_" + m.name + "_", true, m.name, m.size, checks_for(m, programs.back())});
        }
        if (!modules.empty())
            std::printf("%zu modules, code %zu instructions (%zu KiB), %zu sites in the program linked at run time\n", modules.size(), module_code,
                        module_code * 4 / 1024, imports.size());
        const std::vector<Snr2Module> overlays =
            overlays_dir.empty() || !std::filesystem::exists(ps2::os::path(overlays_dir)) ? std::vector<Snr2Module>{} : read_overlays(overlays_dir);
        // Data in a window lives and dies with the game, so captures of one
        // image differ in bytes it wrote since. The code identifies the image:
        // analyse every capture with the same seeds, group the ones answering
        // to one code, and translate one image per group, so a re-captured
        // image is never served by a translation that has not seen its entry
        // points.
        std::size_t overlay_code = 0;
        std::map<std::uint32_t, std::vector<std::uint32_t>> window_seeds;
        std::map<std::uint32_t, std::set<std::uint32_t>> window_tokens;
        std::map<std::string, std::string> variant_of;
        std::map<std::string, std::set<std::uint32_t>> window_offsets;
        std::map<std::uint32_t, std::vector<std::pair<std::string, std::vector<std::pair<std::uint32_t, std::uint32_t>>>>> code_signatures;
        std::set<std::string> representatives;
        for (const Snr2Module &o : overlays) {
            if (!window_seeds.count(o.base)) window_seeds[o.base] = window_entries(image, program, o.base, o.size);
            if (const auto it = entries.modules.find(o.name); it != entries.modules.end())
                for (const std::uint32_t entry : it->second) window_tokens[o.base].insert(entry);
        }
        for (const Snr2Module &o : overlays) {
            Image candidate = Image::raw(o.base, o.bytes, {0, o.size});
            std::vector<std::uint32_t> seeds = window_seeds[o.base];
            for (const std::uint32_t entry : window_tokens[o.base]) seeds.push_back(o.base + entry);
            const Program signature_program = analyse(candidate, seeds, {});
            std::vector<std::pair<std::uint32_t, std::uint32_t>> signature;
            for (std::uint32_t at = 0; at + 4 <= o.size; at += 4)
                if (signature_program.is_code(o.base + at)) {
                    std::uint32_t word = 0;
                    std::memcpy(&word, o.bytes.data() + at, 4);
                    signature.emplace_back(at, word);
                }
            bool found = false;
            for (const auto &[name, sig] : code_signatures[o.base])
                if (sig == signature) { found = true; variant_of[o.name] = name; break; }
            if (!found) {
                code_signatures[o.base].push_back({o.name, signature});
                variant_of[o.name] = o.name;
                representatives.insert(o.name);
            }
            if (const auto it = entries.modules.find(o.name); it != entries.modules.end())
                for (const std::uint32_t entry : it->second) window_offsets[variant_of[o.name]].insert(entry);
        }
        for (const Snr2Module &o : overlays) {
            if (!representatives.count(o.name)) continue;
            images.push_back(Image::raw(o.base, o.bytes, {0, o.size}));
            std::vector<std::uint32_t> seeds = window_seeds[o.base];
            // Entry tokens name module positions, and unlike a relocatable
            // module this image occupies a real address.
            for (const std::uint32_t entry : window_offsets[o.name]) seeds.push_back(o.base + entry);
            programs.push_back(analyse(images.back(), seeds, {}));
            overlay_code += code_size(programs.back());
            translations.push_back({&images.back(), &programs.back(), nullptr, "m_" + o.name + "_", false, o.name, o.size,
                                    checks_for_window(o, programs.back()), true, o.base});
            for (std::uint32_t pc = o.base; pc < o.base + o.size; pc += 4) {
                if (!programs.back().is_code(pc)) continue;
                const Insn i = decode(*images.back().word(pc), pc);
                const Flow f = flow(i.op);
                const std::uint32_t t = i.jump_target();
                if ((f == Flow::Jump || f == Flow::Call) && t >= program.base && t < program.end && !program.entries.count(t)) called_out.insert(t);
            }
        }
        // Window code jumps into the program at places the program may reach
        // only through pointers, if at all: entry points of it all the same.
        if (!called_out.empty()) {
            std::vector<std::uint32_t> seeds = entries.main;
            seeds.insert(seeds.end(), called_out.begin(), called_out.end());
            program = analyse(image, seeds, dynamic_sites(imports));
            std::printf("%zu entry points of the program called from modules and code windows\n", called_out.size());
        }
        if (!overlays.empty()) {
            std::size_t called_entries = 0;
            for (const auto &[base, seeds] : window_seeds) called_entries += seeds.size();
            std::printf("%zu distinct code window images of %zu captured, code %zu instructions (%zu KiB), %zu entry points called from the program\n",
                        std::count_if(overlays.begin(), overlays.end(), [&](const Snr2Module &o) { return representatives.count(o.name) != 0; }),
                        overlays.size(), overlay_code, overlay_code * 4 / 1024, called_entries);
        }

        if (!out.empty()) {
            CodegenOptions options;
            options.output = out;
            if (!probes_path.empty())
                for (std::uint32_t a : read_entries(probes_path).main) options.probes.insert(a);
            if (!toggles_path.empty()) options.toggles = read_toggles(toggles_path);
            generate(translations, options);
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "eerecomp: %s\n", error.what());
        return 1;
    }
    return 0;
}
