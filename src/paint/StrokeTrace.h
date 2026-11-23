// The stroke trace: while the external Stroke Recorder (stroke_rec\) runs, HairHarpy writes into its session every pen / mouse event
// the painting views get and every dab the brush lays — hairharpy_trace.jsonl, timed by the same QPC clock as the recorder's video and
// pen log, so the hand, what the program heard and what the brush did line up to the microsecond. Off (one cheap check a second) when
// the recorder is not running: the recorder's handshake file Documents\HairHarpy Sessions\stroke_rec.json is missing or stale.
#pragma once
#include <QByteArray>
#include <QString>
#include <initializer_list>
#include <utility>

namespace hm::strace {

bool on();                                                         // (the recorder is running — write)
// one line: {"t": <s since the recording began>, "e": kind, name: number…, extra (raw JSON members, no braces)}
void line(const char* kind, std::initializer_list<std::pair<const char*, double>> nums, const QByteArray& extra = {});
QByteArray str(const char* key, const QString& v);                // (a "key": "text" member for extra)
// (held while the program draws a stroke for itself — a brush's thumbnail: none of it in the trace)
struct Mute { Mute(); ~Mute(); Mute(const Mute&) = delete; Mute& operator=(const Mute&) = delete; };

}  // namespace hm::strace
