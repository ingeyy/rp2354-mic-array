#include "speaker_playback.h"
#include "board_pins.h"
#include "audio_config.h"
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/misc/pio_rpi_pico/pio_rpi_pico.h>
#include <hardware/dma.h>
#include <hardware/clocks.h>
#include <hardware/pio_instructions.h>
#include <string.h>
#include <stdlib.h>
#define FRAMES 128u
#define RING 2048u
#define TARGET 1024u
#define DMA_NODE DT_NODELABEL(dma)
#define PLAY_IRQ DT_IRQ_BY_IDX(DMA_NODE,3,irq)
static int16_t ring[RING][2];
static uint32_t blocks[2][FRAMES*2];
static struct k_spinlock lock;
static PIO pio;static size_t sm;
static int channels[2];static uint32_t mask;static unsigned expected;
static struct {bool enabled,primed;unsigned read,fill;int32_t filtered;
 uint32_t received,played,packets,underruns,overruns,faults,blocks,silence,min_fill,max_fill;int peak;
} state;
static __ramfunc __attribute__((optimize("O3"))) void fill_block(unsigned slot)
{
 k_spinlock_key_t key=k_spin_lock(&lock);
 if(state.enabled && !state.primed && state.fill>=TARGET)state.primed=true;
 if(state.enabled && state.primed && state.fill<FRAMES){state.underruns++;state.primed=false;}
 unsigned available=(state.primed || !state.enabled)?MIN(state.fill,FRAMES):0;
 for(unsigned n=0;n<available;n++){
  const int16_t *s=ring[(state.read+n)&(RING-1)];
  blocks[slot][2*n]=(uint32_t)(uint16_t)s[0]<<16;
  blocks[slot][2*n+1]=(uint32_t)(uint16_t)s[1]<<16;
 }
 if(available<FRAMES){memset(blocks[slot]+available*2,0,(FRAMES-available)*8);state.silence+=FRAMES-available;}
 state.read=(state.read+available)&(RING-1);state.fill-=available;state.played+=available;
 if(!state.enabled && !state.fill)state.primed=false;
 if(available)state.min_fill=MIN(state.min_fill,state.fill);
 state.blocks++;k_spin_unlock(&lock,key);
}
static void dma_isr(const void *unused)
{
 ARG_UNUSED(unused);uint32_t pending=dma_hw->ints3&mask;
 dma_hw->ints3=pending;
 if(pending!=BIT(channels[expected])){state.faults++;dma_hw->inte3&=~mask;pio_sm_set_enabled(pio,sm,false);return;}
 unsigned slot=expected;
 if(dma_hw->ch[channels[slot]].ctrl_trig&DMA_CH0_CTRL_TRIG_AHB_ERROR_BITS){state.faults++;dma_hw->inte3&=~mask;pio_sm_set_enabled(pio,sm,false);return;}
 fill_block(slot);
 dma_channel_set_read_addr(channels[slot],blocks[slot],false);
 dma_channel_set_trans_count(channels[slot],FRAMES*2,false);expected^=1;
}
int speaker_playback_init(void)
{
 const struct device *dev=DEVICE_DT_GET(DT_NODELABEL(mic_pio));
 int result=pio_rpi_pico_allocate_sm(dev,&sm);if(result)return result;
 pio=pio_rpi_pico_get_pio(dev);
 const uint16_t instructions[]={
  pio_encode_out(pio_pins,1)|pio_encode_sideset(2,0),
  pio_encode_jmp_x_dec(0)|pio_encode_sideset(2,1),
  pio_encode_out(pio_pins,1)|pio_encode_sideset(2,2),
  pio_encode_set(pio_x,30)|pio_encode_sideset(2,3),
  pio_encode_out(pio_pins,1)|pio_encode_sideset(2,2),
  pio_encode_jmp_x_dec(4)|pio_encode_sideset(2,3),
  pio_encode_out(pio_pins,1)|pio_encode_sideset(2,0),
  pio_encode_set(pio_x,30)|pio_encode_sideset(2,1)};
 struct pio_program program={.instructions=instructions,.length=ARRAY_SIZE(instructions),.origin=-1};
 if(!pio_can_add_program(pio,&program))return -ENOMEM;
 unsigned offset=pio_add_program(pio,&program);pio_sm_config config=pio_get_default_sm_config();
 sm_config_set_wrap(&config,offset,offset+7);sm_config_set_out_pins(&config,SPEAKER_DATA_PIN,1);
 sm_config_set_sideset(&config,2,false,false);sm_config_set_sideset_pins(&config,SPEAKER_BCLK_PIN);
 sm_config_set_out_shift(&config,false,true,32);sm_config_set_fifo_join(&config,PIO_FIFO_JOIN_TX);
 sm_config_set_clkdiv(&config,(float)clock_get_hz(clk_sys)/(PCM_SAMPLE_RATE*128u));
 for(unsigned pin=SPEAKER_DATA_PIN;pin<=SPEAKER_LRC_PIN;pin++)pio_gpio_init(pio,pin);
 pio_sm_init(pio,sm,offset,&config);pio_sm_set_consecutive_pindirs(pio,sm,SPEAKER_DATA_PIN,3,true);
 pio_sm_set_pins_with_mask(pio,sm,0,BIT(SPEAKER_DATA_PIN)|BIT(SPEAKER_BCLK_PIN)|BIT(SPEAKER_LRC_PIN));
 pio_sm_exec(pio,sm,pio_encode_set(pio_x,30));
 for(unsigned i=0;i<2;i++){
  channels[i]=dma_claim_unused_channel(false);if(channels[i]<0)return -EBUSY;mask|=BIT(channels[i]);
 }
 for(unsigned i=0;i<2;i++){
  dma_channel_config config=dma_channel_get_default_config(channels[i]);
  channel_config_set_transfer_data_size(&config,DMA_SIZE_32);channel_config_set_read_increment(&config,true);
  channel_config_set_write_increment(&config,false);channel_config_set_dreq(&config,pio_get_dreq(pio,sm,true));
  channel_config_set_chain_to(&config,channels[i^1]);
  dma_channel_configure(channels[i],&config,&pio->txf[sm],blocks[i],FRAMES*2,false);
 }
 IRQ_CONNECT(PLAY_IRQ,2,dma_isr,NULL,0);dma_hw->ints3=mask;dma_hw->inte3|=mask;irq_enable(PLAY_IRQ);
 dma_start_channel_mask(BIT(channels[0]));pio_sm_set_enabled(pio,sm,true);return 0;
}
void speaker_playback_enable(bool enabled)
{
 k_spinlock_key_t key=k_spin_lock(&lock);state.enabled=enabled;
 if(enabled){state.primed=false;state.read=state.fill=0;}
 state.filtered=TARGET*256;state.min_fill=RING;k_spin_unlock(&lock,key);
}
bool speaker_playback_active(void){k_spinlock_key_t key=k_spin_lock(&lock);bool value=state.enabled;k_spin_unlock(&lock,key);return value;}
__ramfunc __attribute__((optimize("O3"))) void speaker_playback_receive(const int16_t *pcm,unsigned frames)
{
 k_spinlock_key_t key=k_spin_lock(&lock);
 if(state.enabled){
  if(state.fill+frames>RING){state.overruns++;state.primed=false;state.fill=state.read=0;}
  if(frames<=RING){
   for(unsigned n=0;n<frames;n++){
    unsigned write=(state.read+state.fill+n)&(RING-1);
    ring[write][0]=pcm[2*n];ring[write][1]=pcm[2*n+1];
    state.peak=MAX(state.peak,MAX(abs((int)pcm[2*n]),abs((int)pcm[2*n+1])));
   }
   state.fill+=frames;state.received+=frames;state.packets++;state.max_fill=MAX(state.max_fill,state.fill);
  }
 }
 k_spin_unlock(&lock,key);
}
uint32_t speaker_playback_feedback(void)
{
 k_spinlock_key_t key=k_spin_lock(&lock);
 state.filtered+=((int32_t)state.fill*256-state.filtered)/64;
 int32_t correction=state.primed?CLAMP((state.filtered/256-(int32_t)TARGET)*2,-1638,1638):0;
 k_spin_unlock(&lock,key);return (48u<<14)-correction;
}
static int stats(const struct shell *sh,size_t argc,char **argv)
{
 ARG_UNUSED(argc);ARG_UNUSED(argv);k_spinlock_key_t key=k_spin_lock(&lock);__typeof__(state) s=state;k_spin_unlock(&lock,key);
 shell_print(sh,"SPEAKER enabled=%u primed=%u fill=%u packets=%u received=%u played=%u underruns=%u overruns=%u faults=%u silence=%u min_fill=%u max_fill=%u peak=%d rate=48000 channels=2",s.enabled,s.primed,s.fill,s.packets,s.received,s.played,s.underruns,s.overruns,s.faults,s.silence,s.min_fill,s.max_fill,s.peak);return 0;
}
SHELL_CMD_REGISTER(spkstats,NULL,"UAC2 playback / PIO I2S DMA health",stats);
