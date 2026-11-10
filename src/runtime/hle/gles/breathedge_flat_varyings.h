// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from the RVR-Venture Breathedge GLES flat-varying workaround.
#pragma once
#include <regex>
#include <string>

// GLSL ES requires flat interpolation for integer/bool varyings. Match complete
// global declarations only; never parameters, vertex inputs or fragment outputs.
inline std::string breathedge_flat_varyings(const std::string& input, bool vertex)
{
    static const std::regex declaration(
        R"((^|\n)([ \t]*(?:layout[ \t]*\([^\n)]*\)[ \t]*)?)(flat[ \t]+|smooth[ \t]+|noperspective[ \t]+)?(in|out)([ \t]+(?:(?:lowp|mediump|highp)[ \t]+)?(?:int|uint|bool|[iub]vec[234])[ \t]+[A-Za-z_][A-Za-z_0-9]*[ \t]*(?:\[[^\n\]]*\][ \t]*)?;))");
    std::string result;
    size_t end = 0;
    for (auto it = std::sregex_iterator(input.begin(), input.end(), declaration);
         it != std::sregex_iterator(); ++it) {
        const auto& match = *it;
        const auto pos = static_cast<size_t>(match.position());
        result.append(input, end, pos - end);
        const bool varying = match[4] == (vertex ? "out" : "in");
        if (varying && match[3].str().find("flat") != 0)
            result += match[1].str() + match[2].str() + "flat " + match[4].str() + match[5].str();
        else result += match.str();
        end = pos + static_cast<size_t>(match.length());
    }
    result.append(input, end, std::string::npos);
    return result;
}
