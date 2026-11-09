// MP3 播放器对外接口。
//
// 播放器自己持有解码任务和文件句柄，osd.cpp 只通过这些函数驱动它。
//
// 【I2S 所有权】
//   菜单/音乐页里 I2S 归播放器用（歌曲自己的采样率，实测 44100Hz）；
//   进游戏前必须调 mp3_suspend() 把通道让出来，osd_init_sound() 才能用
//   22050Hz 重新打开给 NES。退出游戏后调 mp3_resume() 恢复。
#ifndef MP3_PLAYER_H
#define MP3_PLAYER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 扫 SD 根目录的 .mp3（幂等，第一次调用才真正扫）
void mp3_scan(void);

int         mp3_count(void);
const char *mp3_name(int i);          // 显示用文件名（不含路径和后缀）

// 播放控制
bool mp3_play_index(int i);           // 播放第 i 首（会先停当前）
void mp3_toggle(void);                // 播放 / 暂停
void mp3_next(void);
void mp3_prev(void);
void mp3_stop(void);

// 状态查询（给 UI 用，全部是廉价读取）
int  mp3_current(void);               // 当前曲目索引，-1 = 没有
bool mp3_is_playing(void);            // 正在放（含暂停态）
bool mp3_is_paused(void);
int  mp3_progress_pct(void);          // 0..100，按"已读文件字节"估算

// I2S 所有权转移
void mp3_suspend(void);               // 进游戏前：停播放 + 关 I2S
void mp3_resume(void);                // 退出游戏后：重新打开 I2S（如果之前在放）

#ifdef __cplusplus
}
#endif

#endif
