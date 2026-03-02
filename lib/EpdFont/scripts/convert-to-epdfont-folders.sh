#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
BOOKERLY_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)
OPENDYSLEXIC_FONT_SIZES=(8 10 12 14)
UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

# Output base directory
OUTPUT_BASE="fonts_preconverted"
mkdir -p "$OUTPUT_BASE"

convert_family() {
    local family_name=$1
    local source_dir=$2
    local prefix=$3
    local ext=$4
    shift 4
    local sizes=("$@")

    echo "Converting $family_name..."
    local out_dir="$OUTPUT_BASE/$family_name"
    mkdir -p "$out_dir"

    for size in "${sizes[@]}"; do
        for style in "${READER_FONT_STYLES[@]}"; do
            local style_lower=$(echo "$style" | tr '[:upper:]' '[:lower:]')
            local font_path="../builtinFonts/source/$source_dir/${prefix}-${style}.${ext}"
            
            # Use NotoSans-Regular as fallback for italic if not present (OpenDyslexic e.g.)
            if [ ! -f "$font_path" ]; then
                if [ "$style" == "Italic" ]; then
                   font_path="../builtinFonts/source/$source_dir/${prefix}-Regular.${ext}"
                elif [ "$style" == "BoldItalic" ]; then
                   font_path="../builtinFonts/source/$source_dir/${prefix}-Bold.${ext}"
                fi
            fi

            if [ -f "$font_path" ]; then
                local out_file="$out_dir/${size}_${style_lower}.epdfont"
                python3 fontconvert.py "$family_name" "$size" "$font_path" --2bit --bin "$out_file"
                echo "  Generated $out_file"
            fi
        done
    done
}

convert_family "Bookerly" "Bookerly" "Bookerly" "ttf" "${BOOKERLY_FONT_SIZES[@]}"
convert_family "NotoSans" "NotoSans" "NotoSans" "ttf" "${NOTOSANS_FONT_SIZES[@]}"
convert_family "OpenDyslexic" "OpenDyslexic" "OpenDyslexic" "otf" "${OPENDYSLEXIC_FONT_SIZES[@]}"

# UI fonts (Ubuntu) only have Regular/Bold usually
echo "Converting Ubuntu..."
U_DIR="$OUTPUT_BASE/Ubuntu"
mkdir -p "$U_DIR"
for size in "${UI_FONT_SIZES[@]}"; do
    for style in "${UI_FONT_STYLES[@]}"; do
        style_lower=$(echo "$style" | tr '[:upper:]' '[:lower:]')
        font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
        if [ -f "$font_path" ]; then
            out_file="$U_DIR/${size}_${style_lower}.epdfont"
            python3 fontconvert.py "Ubuntu" "$size" "$font_path" --bin "$out_file"
            echo "  Generated $out_file"
        fi
    done
done

echo ""
echo "All fonts converted to $OUTPUT_BASE"
echo "Copy these folders to the /fonts/ directory on your SD card."
