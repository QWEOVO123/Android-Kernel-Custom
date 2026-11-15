/* Execute the real renderer against a guarded 1600x2560 RGBX framebuffer. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../bootmenu-source/KernelSU/kernel/preinit/runner/bootmenu/model.h"
struct bm_buffer { unsigned *pixels; unsigned pitch; };
struct bm_display { int width,height,current; struct bm_buffer buffers[2]; };
#include "../bootmenu-source/KernelSU/kernel/preinit/runner/bootmenu/render.h"
static unsigned at(struct bm_display *d,int x,int y)
{
    int row=x*2560/1280, column=1599-y*1600/800;
    return d->buffers[0].pixels[row*1600+column];
}
int main(int argc,char **argv)
{
    size_t count=1600*2560;
    unsigned *guarded=malloc((count+32)*sizeof(unsigned)); assert(guarded);
    for (size_t i=0;i<count+32;i++) guarded[i]=0x12345678;
    struct bm_display d={.width=1600,.height=2560,.buffers={{guarded+16,6400}}};
    struct bm_model model={.focus=BM_START_TARGET,.sukisu_allowed=1,.tee_available=1};
    bm_render(&d,&model,BM_KEY_VOLUME|BM_KEY_POWER,1,60);
    assert(at(&d,0,0)==bm_pack(0x111e35));
    assert(at(&d,640,bm_targets[BM_START_TARGET].y)==bm_pack(0xd5e8ff));
    model.page=BM_TAB_SU;
    model.features=BM_ROOT_MEOW_SU;
    model.focus=BM_TAB_SU;
    bm_render(&d,&model,BM_KEY_VOLUME|BM_KEY_POWER,1,59);
    assert(at(&d,640,bm_targets[BM_CUSTOM_TARGET].y)==bm_pack(0x527aba));
    model.page=BM_TAB_SECURITY;
    model.features=BM_SECURITY_MASK;
    model.focus=BM_TAB_SECURITY;
    bm_render(&d,&model,BM_KEY_VOLUME|BM_KEY_POWER,1,58);
    for (int row=BM_BOOTLOADER_TARGET;row<=BM_CUSTOM_TARGET;row++)
        assert(at(&d,640,bm_targets[row].y)==bm_pack(0x527aba));
    for (int i=0;i<16;i++) assert(guarded[i]==0x12345678 && guarded[count+16+i]==0x12345678);
    if (argc==2) {
        FILE *f=fopen(argv[1],"wb"); assert(f);
        fprintf(f,"P6\n1280 800\n255\n");
        unsigned char row[1280*3];
        for (int y=0;y<800;y++) {
            for (int x=0;x<1280;x++) {
                unsigned color=at(&d,x,y);
                row[x*3]=color;row[x*3+1]=color>>8;row[x*3+2]=color>>16;
            }
            assert(fwrite(row,1,sizeof(row),f)==sizeof(row));
        }
        fclose(f);
    }
    free(guarded);
    puts("Modern renderer: rotated buffer bounds, focus, switch state and glyph rendering passed");
    return 0;
}
