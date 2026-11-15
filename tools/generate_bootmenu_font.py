"""Generate a redistributable Chinese/Latin bitmap subset, without a compiler."""
from pathlib import Path
import hashlib
import urllib.request
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parents[1]
assets = root / 'reference/bootmenu-font'
assets.mkdir(parents=True, exist_ok=True)
base = 'https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/'
for remote, local in [('NotoSansSC%5Bwght%5D.ttf', 'NotoSansSC.ttf'), ('OFL.txt', 'OFL.txt')]:
    target = assets / local
    if not target.exists():
        target.write_bytes(urllib.request.urlopen(base + remote, timeout=45).read())
font = ImageFont.truetype(str(assets / 'NotoSansSC.ttf'), 48)
font.set_variation_by_axes([500])
labels = ('QWEOVO Boot Menu SukiSU Root TTL CPU Bootloader Recovery '
          '启用授权与模块自定义免功能优先级启动关机重到每次开重新选择'
          '音量键切换电源确认触摸点击个开关后按继续等待设备准备失败'
          '两项默认关闭秒无操作自动进入系统本次设置仅有效不保存'
          '启动菜单返回取消确定请稍候显示触摸未就绪使用按键当前'
          '，。·：/（）0123456789+-')
chars = sorted(set(labels), key=ord)
render = root / 'bootmenu-source/KernelSU/kernel/preinit/runner/bootmenu/render.h'
if render.exists():
    labels += ''.join(ch for ch in render.read_text(encoding='utf-8') if ord(ch) >= 128)
chars = sorted(set(labels) | set(chr(code) for code in range(32,127)), key=ord)
records, data = [], bytearray()
for ch in chars:
    left, top, right, bottom = font.getbbox(ch, anchor='ls')
    w, h = max(0, right-left), max(0, bottom-top)
    image = Image.new('L', (max(w, 1), max(h, 1)))
    ImageDraw.Draw(image).text((-left, -top), ch, fill=255, font=font, anchor='ls')
    records.append((ord(ch), left, top, w, h, round(font.getlength(ch)), len(data)))
    data.extend(image.tobytes()[:w*h])
out = root / 'bootmenu-source/KernelSU/kernel/preinit/runner/bootmenu'
out.mkdir(parents=True, exist_ok=True)
header = ['/* Generated glyph subset of Noto Sans SC; SIL OFL 1.1, see FONT-LICENSE.txt. */',
          '/* Source font SHA256: '+hashlib.sha256((assets/'NotoSansSC.ttf').read_bytes()).hexdigest()+' */',
          '#ifndef QWEOVO_BOOTMENU_FONT_H', '#define QWEOVO_BOOTMENU_FONT_H',
          'struct bm_glyph { unsigned code; short left, top; unsigned short w, h, advance; unsigned offset; };',
          'static const struct bm_glyph bm_glyphs[] = {']
header += ['    {%u,%d,%d,%u,%u,%u,%u},' % row for row in records]
header += ['};', 'static const unsigned char bm_font_pixels[] = {']
header += ['    '+','.join(str(x) for x in data[i:i+32])+',' for i in range(0,len(data),32)]
header += ['};', '#endif', '']
(out/'font.h').write_text('\n'.join(header))
(out/'FONT-LICENSE.txt').write_bytes((assets/'OFL.txt').read_bytes())
print('Generated', len(records), 'glyphs,', len(data), 'alpha bytes; no compilation performed.')
