# LVGL Image Conversion Tools

This directory contains two Python scripts for processing and converting images to LVGL format:

## 1. LVGLImage (LVGLImage.py)

Referenced from LVGL [official repo](https://github.com/lvgl/lvgl)'s conversion script [LVGLImage.py](https://github.com/lvgl/lvgl/blob/master/scripts/LVGLImage.py)

## 2. LVGL Image Conversion Tool (lvgl_tools_gui.py)

Calls `LVGLImage.py` to batch convert images to LVGL image format.
Can be used to modify XiaoZhi's default expressions, detailed modification tutorial [here](https://www.bilibili.com/video/BV12FQkYeEJ3/)

### Features

- Graphical interface, more user-friendly
- Supports batch image conversion
- Automatically detects image format and selects the best color format for conversion
- Multiple resolution support

### Usage

Create virtual environment
```bash
# Create venv
python -m venv venv
# Activate environment
source venv/bin/activate  # Linux/Mac
venv\Scripts\activate     # Windows
```

Install dependencies
```bash
pip install -r requirements.txt
```

Run conversion tool
```bash
# Activate environment  
source venv/bin/activate  # Linux/Mac
venv\Scripts\activate     # Windows
# Run
python lvgl_tools_gui.py
```

