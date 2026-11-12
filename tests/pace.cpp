// The picture pace's rule (runtime/include/ps2/pace.hpp), no game needed: ninja ps2pacetest && ./ps2pacetest
// A made-up game is driven through it: it works for a while after each picture, waits for the next vertical blank and shows the
// next picture there; a hold makes it go on only at the vertical blank the rule names.
#include "ps2/pace.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int g_failed = 0;

void check(bool ok, const char *what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

struct Run {
    std::vector<std::uint64_t> pictures;  // the refresh of each picture
    int holds = 0;
};

// `work`: refreshes of work after a picture before the game waits for the next vertical blank. Refresh k starts at time k.
Run play(int every, double work, int count) {
    ps2::pace::Hold hold;
    hold.every = every;
    Run run;
    double t = 0.25;  // the game starts a quarter into refresh 0
    for (int i = 0; i < count; ++i) {
        const auto shown = static_cast<std::uint64_t>(std::floor(t + work)) + 1;  // the vertical blank after its work
        run.pictures.push_back(shown);
        const std::uint64_t until = hold.picture(shown);
        hold.picture(shown);  // the second display buffer register in the same refresh: the same picture, no second hold
        if (until) ++run.holds, t = static_cast<double>(until);
        else t = static_cast<double>(shown);
    }
    return run;
}

// Every gap between two pictures after the first few, which settle.
bool gaps_are(const Run &r, std::uint64_t gap) {
    for (std::size_t i = 3; i < r.pictures.size(); ++i)
        if (r.pictures[i] - r.pictures[i - 1] != gap) return false;
    return true;
}

}  // namespace

int main() {
    // A game whose work fits in one refresh shows a picture every refresh; held to 30 it shows one every second refresh.
    const Run off = play(1, 0.4, 40), on = play(2, 0.4, 40);
    check(gaps_are(off, 1) && off.holds == 0, "off: a quick game shows a picture every refresh, never held");
    check(gaps_are(on, 2), "pace 30: a quick game shows a picture every second refresh");
    check(on.holds >= 38, "pace 30: a quick game is held once for every picture");
    // 30 pictures a second: once settled (the first hold comes at the second picture), 30 more pictures take 60 refreshes
    const Run sixty = play(2, 0.4, 32);
    check(sixty.pictures[31] - sixty.pictures[1] == 60, "pace 30: 30 pictures in 60 refreshes");
    // A game that already needs more than one refresh is not slowed further.
    const Run slow = play(2, 1.5, 40), slow_off = play(1, 1.5, 40);
    check(gaps_are(slow, 2) && slow.holds == 0 && slow.pictures == slow_off.pictures, "pace 30: a game at 30 on its own runs the same, never held");
    const Run slower = play(2, 2.5, 40), slower_off = play(1, 2.5, 40);
    check(gaps_are(slower, 3) && slower.holds == 0 && slower.pictures == slower_off.pictures, "pace 30: a game at 20 stays at 20, never held");
    // A game that is quick in some pictures and slow in others: held only in the quick ones.
    {
        ps2::pace::Hold hold;
        hold.every = 2;
        std::uint64_t t = 0;
        int held = 0;
        const double works[] = {0.3, 0.3, 1.6, 0.3, 2.2, 0.3, 0.3};
        std::vector<std::uint64_t> shown;
        double now = 0.1;
        for (double w : works) {
            const auto at = static_cast<std::uint64_t>(std::floor(now + w)) + 1;
            shown.push_back(at);
            t = hold.picture(at);
            if (t) ++held, now = static_cast<double>(t);
            else now = static_cast<double>(at);
        }
        // A picture is shown at once and the game held after it, so a quick picture right after a slow one comes one refresh
        // later; the game is held then and the next quick one is two refreshes on again.
        const std::vector<std::uint64_t> expected = {1, 2, 5, 6, 10, 11, 13};
        check(shown == expected && held == 4, "pace 30: mixed quick and slow pictures, held only after the quick ones");
    }
    check(gaps_are(play(3, 0.4, 40), 3), "pace 20: a quick game shows a picture every third refresh");

    // The end-of-run count.
    {
        ps2::pace::Count n;
        for (std::uint64_t f : {1, 2, 2, 4, 7, 12}) n.picture(f);
        check(n.pictures == 5 && n.gaps[0] == 1 && n.gaps[1] == 1 && n.gaps[2] == 1 && n.gaps[3] == 1, "count: five pictures, one gap of each size");
    }

    // The setting and the built-in list.
    check(ps2::pace::parse("30") == 2 && ps2::pace::parse(" 30 flip ") == 2 && ps2::pace::parse("20") == 3 && ps2::pace::parse("15") == 4,
          "setting: 30, '30 flip', 20, 15");
    check(ps2::pace::parse("off") == 1 && ps2::pace::parse("60") == 1 && ps2::pace::parse("OFF") == 1, "setting: off and 60 turn it off");
    check(!ps2::pace::parse("45") && !ps2::pace::parse("30 copy") && !ps2::pace::parse(""), "setting: 45, '30 copy' and nothing are not settings");
    check(ps2::pace::serial_of("games/hsx/disc/SLUS_205.92") == "SLUS-20592" && ps2::pace::serial_of("cdrom0:\\SLUS_205.92;1") == "SLUS-20592",
          "serial: SLUS_205.92 -> SLUS-20592");
    check(ps2::pace::serial_of("ps2run.elf").empty(), "serial: a name that is not one");
    const ps2::pace::Listed *hsx = ps2::pace::listed("SLUS-20592");
    check(hsx && hsx->every == 2, "list: HSX Hypersonic Xtreme (USA) gets one picture every 2 refreshes");
    check(!ps2::pace::listed("SCUS-97481"), "list: God of War II (USA) is not listed");

    std::printf("%s\n", g_failed ? "FAILED" : "all passed");
    return g_failed ? 1 : 0;
}
