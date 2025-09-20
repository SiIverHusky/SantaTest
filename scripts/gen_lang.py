#!/usr/bin/env python3
import argparse
import json
import os

HEADER_TEMPLATE = """// Auto-generated language config
#pragma once

#include <string_view>

#ifndef {lang_code_for_font}
    #define {lang_code_for_font}  // Default language
#endif

namespace Lang {{
    // Language metadata
    constexpr const char* CODE = "{lang_code}";

    // String resources
    namespace Strings {{
{strings}
    }}

    // Sound resources
    namespace Sounds {{
{sounds}
    }}
}}
"""

def generate_header(input_path, output_path):
    with open(input_path, 'r', encoding='utf-8') as f:
        data = json.load(f)

    # Validate data structure
    if 'language' not in data or 'strings' not in data:
        raise ValueError("Invalid JSON structure")

    lang_code = data['language']['type']

    # Generate string constants
    strings = []
    sounds = []
    for key, value in data['strings'].items():
        value = value.replace('"', '\\"')
        strings.append(f'        constexpr const char* {key.upper()} = "{value}";')

    # Generate sound effect constants
    for file in os.listdir(os.path.dirname(input_path)):
        if file.endswith('.p3'):
            base_name = os.path.splitext(file)[0]
            sounds.append(f'''
        extern const char p3_{base_name}_start[] asm("_binary_{base_name}_p3_start");
        extern const char p3_{base_name}_end[] asm("_binary_{base_name}_p3_end");
        static const std::string_view P3_{base_name.upper()} {{
        static_cast<const char*>(p3_{base_name}_start),
        static_cast<size_t>(p3_{base_name}_end - p3_{base_name}_start)
        }};''')
    
    # Generate common sound effects
    for file in os.listdir(os.path.join(os.path.dirname(output_path), 'common')):
        if file.endswith('.p3'):
            base_name = os.path.splitext(file)[0]
            sounds.append(f'''
        extern const char p3_{base_name}_start[] asm("_binary_{base_name}_p3_start");
        extern const char p3_{base_name}_end[] asm("_binary_{base_name}_p3_end");
        static const std::string_view P3_{base_name.upper()} {{
        static_cast<const char*>(p3_{base_name}_start),
        static_cast<size_t>(p3_{base_name}_end - p3_{base_name}_start)
        }};''')

    # Fill template
    content = HEADER_TEMPLATE.format(
        lang_code=lang_code,
        lang_code_for_font=lang_code.replace('-', '_').lower(),
        strings="\n".join(sorted(strings)),
        sounds="\n".join(sorted(sounds))
    )

    # Write to file
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    with open(output_path, 'w', encoding='utf-8') as f:
        f.write(content)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, help="Input JSON file path")
    parser.add_argument("--output", required=True, help="Output header file path")
    args = parser.parse_args()

    generate_header(args.input, args.output)