/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef QWEOVO_BOOTMENU_RENDER_H
#define QWEOVO_BOOTMENU_RENDER_H
#include "font.h"

static unsigned bm_pack(unsigned rgb)
{
    return 0xff000000U | ((rgb>>16)&255) | (rgb&0xff00) | ((rgb&255)<<16);
}

/* Rotation 90, consistent with swap-XY/flip-Y touch coordinates. */
static void bm_pixel(struct bm_display *d, int x, int y, unsigned color)
{
    unsigned *pixels=d->buffers[d->current].pixels;
    if (x<0 || y<0 || x>=d->height || y>=d->width) return;
    pixels[x*(d->buffers[d->current].pitch/4)+(d->width-1-y)]=color;
}

static unsigned *bm_pixel_address(struct bm_display *d, int x, int y)
{
    if (x<0 || y<0 || x>=d->height || y>=d->width) return NULL;
    return &d->buffers[d->current].pixels[x*(d->buffers[d->current].pitch/4)+(d->width-1-y)];
}

static void bm_round(struct bm_display *d, struct bm_rect r, int radius, unsigned rgb)
{
    int x0=r.x*d->height/BM_WIDTH, y0=r.y*d->width/BM_HEIGHT;
    int x1=(r.x+r.w)*d->height/BM_WIDTH, y1=(r.y+r.h)*d->width/BM_HEIGHT;
    int rad=radius*d->width/BM_HEIGHT, x,y;
    unsigned color=bm_pack(rgb);
    for (y=y0; y<y1; y++) for (x=x0; x<x1; x++) {
        int dx=x<x0+rad ? x0+rad-x : x>=x1-rad ? x-(x1-rad-1) : 0;
        int dy=y<y0+rad ? y0+rad-y : y>=y1-rad ? y-(y1-rad-1) : 0;
        if (dx*dx+dy*dy<=rad*rad) bm_pixel(d,x,y,color);
    }
}

static unsigned bm_utf8(const char **text)
{
    const unsigned char *p=(const unsigned char *)*text;
    unsigned code;
    int n,i;
    if (*p<128) { *text+=1; return *p; }
    if ((*p&0xe0)==0xc0) { code=*p&31; n=2; }
    else if ((*p&0xf0)==0xe0) { code=*p&15; n=3; }
    else if ((*p&0xf8)==0xf0) { code=*p&7; n=4; }
    else { *text+=1; return '?'; }
    for (i=1; i<n; i++) {
        if (!p[i] || (p[i]&0xc0)!=0x80) { *text+=1; return '?'; }
        code=(code<<6)|(p[i]&63);
    }
    *text+=n;
    return code;
}

static const struct bm_glyph *bm_find_glyph(unsigned code)
{
    int low=0, high=sizeof(bm_glyphs)/sizeof(bm_glyphs[0])-1;
    while (low<=high) {
        int mid=(low+high)/2;
        if (bm_glyphs[mid].code==code) return &bm_glyphs[mid];
        if (bm_glyphs[mid].code<code) low=mid+1; else high=mid-1;
    }
    return NULL;
}

static int bm_text_width(const char *text, int size)
{
    int advance=0;
    while (*text) {
        const struct bm_glyph *g=bm_find_glyph(bm_utf8(&text));
        if (g) advance+=g->advance;
    }
    return advance*size/48;
}

static void bm_text(struct bm_display *d, int x, int y, const char *text,
                     int size, unsigned rgb)
{
    long advance=0;
    unsigned src=bm_pack(rgb);
    while (*text) {
        const struct bm_glyph *g=bm_find_glyph(bm_utf8(&text));
        int x0,y0,w,h,dx,dy;
        if (!g) continue;
        x0=((long)x*48+(advance+g->left)*size)*d->height/(48*BM_WIDTH);
        y0=((long)(y+size)*48+g->top*size)*d->width/(48*BM_HEIGHT);
        w=(long)g->w*size*d->height/(48*BM_WIDTH);
        h=(long)g->h*size*d->width/(48*BM_HEIGHT);
        for (dy=0; dy<h; dy++) for (dx=0; dx<w; dx++) {
            unsigned alpha=bm_font_pixels[g->offset+(dy*g->h/h)*g->w+(dx*g->w/w)];
            unsigned *pixel;
            unsigned r,gc,b,old;
            if (!alpha || !(pixel=bm_pixel_address(d,x0+dx,y0+dy))) continue;
            old=*pixel;
            r=((src&255)*alpha+(old&255)*(255-alpha))/255;
            gc=(((src>>8)&255)*alpha+((old>>8)&255)*(255-alpha))/255;
            b=(((src>>16)&255)*alpha+((old>>16)&255)*(255-alpha))/255;
            *pixel=0xff000000U|r|(gc<<8)|(b<<16);
        }
        advance+=g->advance;
    }
}

static void bm_center(struct bm_display *d, struct bm_rect r, const char *text,
                       int size, unsigned color)
{
    bm_text(d,r.x+(r.w-bm_text_width(text,size))/2,r.y+(r.h-size)/2-2,text,size,color);
}

static void bm_wrench(struct bm_display *d, int x, int y, int size, unsigned rgb)
{
    static const int shape[][2]={
        {5,0},{10,0},{14,4},{14,9},{23,18},{23,21},{21,23},
        {18,23},{9,14},{4,14},{0,10},{0,5},{5,10},{9,6}
    };
    for (int py=0;py<size;py++) for (int px=0;px<size;px++) {
        int sx=px*48/size+1, sy=py*48/size+1, inside=0;
        for (unsigned i=0,j=sizeof(shape)/sizeof(shape[0])-1;
             i<sizeof(shape)/sizeof(shape[0]);j=i++) {
            int ax=shape[i][0]*2,ay=shape[i][1]*2;
            int bx=shape[j][0]*2,by=shape[j][1]*2;
            if ((ay>sy)!=(by>sy))
                if (sx<(bx-ax)*(sy-ay)/(by-ay)+ax) inside=!inside;
        }
        if (inside) bm_round(d,(struct bm_rect){x+px,y+py,1,1},0,rgb);
    }
    bm_round(d,(struct bm_rect){x+size*19/24,y+size*19/24,size/8,size/8},size/16,0x9ec5ff);
}

static void bm_render(struct bm_display *d, const struct bm_model *m,
                       int keys, int has_touch, int remaining)
{
    unsigned *pixels=d->buffers[d->current].pixels;
    unsigned stride=d->buffers[d->current].pitch/4;
    int x,y,i;
    const char *hint="Use the touch screen to select";
    char note[96];
    static const char *const headings[]={"Boot options","Root access","Security"};

    for (y=0; y<d->height; y++) for (x=0; x<d->width; x++)
        pixels[y*stride+x]=bm_pack(0x0b1426);
    bm_round(d,(struct bm_rect){0,0,1280,124},0,0x111e35);
    bm_round(d,(struct bm_rect){0,122,1280,2},0,0x34547f);
    bm_round(d,(struct bm_rect){72,32,60,60},14,0x9ec5ff);
    bm_wrench(d,85,45,34,0x102753);
    bm_text(d,151,32,"Meow BootMenu",36,0xf0f5ff);
    bm_text(d,153,79,"TB320FC",17,0x9ab1d2);
    bm_round(d,(struct bm_rect){72,148,228,480},12,0x101e34);
    bm_round(d,(struct bm_rect){336,148,872,480},12,0x101e34);
    bm_text(d,352,160,headings[m->page],26,0xf0f5ff);
    bm_round(d,(struct bm_rect){352,197,840,1},0,0x304565);
    for (i=0; i<BM_TARGETS; i++) {
        static const char *const tabs[]={"Boot","SU","Security"};
        static const char *const boot[]={"Bootloader","Power off",
            "Custom features","Status Spoofer","TEE Simulator"};
        static const char *const su[]={"Root off","SukiSU","MeowSU"};
        static const char *const security[]={"Lock SELinux","Lock Kernel Modules","Security Audit"};
        struct bm_rect r=bm_targets[i], inside={r.x+2,r.y+2,r.w-4,r.h-4};
        const char *label, *status="";
        int locked, enabled=0, selected=i==m->page;

        if (!bm_visible(m,i)) continue;
        if (i==BM_START_TARGET) {
            bm_round(d,r,12,m->focus==i ? 0xd5e8ff : 0x629af0);
            bm_round(d,inside,10,0x9ec5ff);
            bm_center(d,r,"Start Android",31,0x102753);
            continue;
        }
        locked=bm_locked(m,i);
        if (i<BM_FIRST_ROW) {
            label=tabs[i];
        } else {
            unsigned flag=bm_row_flag(m,i);
            int row=i-BM_BOOTLOADER_TARGET;

            if (m->page==BM_TAB_SU) {
                label=su[row];
                enabled=flag ? !!(m->features&flag) : !(m->features&BM_ROOT_MASK);
                if (enabled) status="Selected";
                if (locked) status=(m->features&BM_SECURITY_MASK) ? "Security enabled" : "Authorization required";
            } else if (m->page==BM_TAB_SECURITY) {
                label=security[row];
                enabled=!!(m->features&flag);
                status=locked ? "Requires Root off" : enabled ? "On" : "Off";
            } else {
                label=boot[row];
                enabled=!!(m->features&flag);
                if (flag) status=locked ? "Unavailable" : enabled ? "On" : "Off";
            }
        }
        bm_round(d,r,7,m->focus==i ? 0xd5e8ff : selected||enabled ? 0x527aba : 0x293e5d);
        bm_round(d,inside,5,selected||enabled ? 0x193457 : 0x17263e);
        if (i<BM_FIRST_ROW) {
            if (selected)
                bm_round(d,(struct bm_rect){r.x+3,r.y+9,4,r.h-18},0,0x9ec5ff);
            bm_text(d,r.x+21,r.y+14,label,22,selected ? 0x9ec5ff : 0xe1ecff);
        } else {
            bm_text(d,r.x+25,r.y+12,label,24,locked ? 0x829abc : 0xf0f5ff);
            bm_center(d,(struct bm_rect){r.x+r.w-315,r.y,290,r.h},
                      status,17,enabled ? 0x9ec5ff : 0xa6b9d9);
        }
    }
    if (keys==(BM_KEY_VOLUME|BM_KEY_POWER)) hint="Volume to move / Power to select";
    else if (keys==BM_KEY_VOLUME) hint="Volume to move / Hold to select";
    else if (keys==BM_KEY_POWER) hint="Power to move / Double press to select";
    else if (!has_touch) hint="Initializing touch...";
    bm_text(d,72,665,hint,15,0xb8c9e3);
    if (!has_touch && keys) bm_text(d,72,692,"Initializing touch...",15,0x829abc);
    bm_round(d,(struct bm_rect){72,757,1136,1},0,0x304565);
    if (remaining >= 0)
        snprintf(note,sizeof(note),"Auto-start in %d s",remaining);
    else
        snprintf(note,sizeof(note),"Waiting for input");
    bm_text(d,72,769,note,16,0x9ab1d2);
}
#endif
