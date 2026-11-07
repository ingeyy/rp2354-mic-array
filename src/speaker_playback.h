#ifndef SPEAKER_PLAYBACK_H
#define SPEAKER_PLAYBACK_H
#include <stdbool.h>
#include <stdint.h>
int speaker_playback_init(void);
void speaker_playback_enable(bool enabled);
bool speaker_playback_active(void);
void speaker_playback_receive(const int16_t *pcm,unsigned frames);
uint32_t speaker_playback_feedback(void);
#endif
