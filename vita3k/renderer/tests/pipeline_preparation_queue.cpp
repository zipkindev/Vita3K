// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <renderer/exact_pipeline_recipes.h>
#include <renderer/pipeline_preparation_queue.h>

int main() {
    renderer::PipelinePreparationQueue<int> queue;
    assert(queue.push(1, false));
    assert(queue.push(2, true));
    assert(queue.push(3, true));
    assert(queue.pop() == 2);
    assert(queue.pop() == 3);
    assert(queue.pop() == 1);
    assert(queue.empty());
    assert(queue.push(10, false));
    assert(queue.push(20, false));
    assert(queue.push(30, false, true));
    assert(queue.promote(20));
    assert(!queue.promote(20));
    assert(queue.push(40, true));
    assert(queue.pop() == 40); // learned exact still outranks actual pairs
    assert(queue.pop() == 30);
    assert(queue.pop() == 20); // upgraded hint retains one owner
    assert(queue.pop() == 10);
    assert(!queue.promote(99));
    for (int i = 0; i < 48; ++i)
        assert(queue.push(i, false, true, true));
    assert(!queue.has_urgent());
    assert(queue.push(90, false, true));
    assert(queue.push(91, true));
    assert(queue.pop() == 91); // learned replay retains priority
    assert(queue.pop() == 90); // fresh actual pair jumps the retry backlog
    for (int i = 0; i < 48; ++i)
        assert(queue.pop() == i);
    assert(queue.empty());
    for (int i = 0; i < 64; ++i)
        assert(queue.push(i, true));
    assert(!queue.push(99, true));
    assert(queue.pop() == 0);
    assert(queue.push(64, true));
    for (int i = 1; i <= 64; ++i)
        assert(queue.pop() == i);
    assert(queue.concurrency(1000000000, 0) == 2);
    assert(queue.concurrency(1000000000, 900000000) == 1);
    assert(queue.concurrency(1150000000, 900000000) == 2);
    assert(queue.concurrency(3000000000, 900000000, true) == 1); // blocked draw is not idle

    renderer::ExactPipelineRecipe a, b;
    a.words[8] = 0; // one-sided: back-face state does not affect pipeline creation
    a.words[9] = a.words[11] = a.words[21] = a.words[23] = a.words[25] = 1;
    a.words[16] = 2;
    a.words[30] = a.words[46] = 7; // used texture unit
    a.words[31] = a.words[47] = 8; // unused texture unit
    b = a;
    b.words[9] = b.words[11] = b.words[21] = b.words[23] = b.words[25] = 2;
    b.words[16] = 3;
    b.words[31] = b.words[47] = 9;
    renderer::canonicalize_exact_pipeline(a, 1, 1, 10);
    renderer::canonicalize_exact_pipeline(b, 1, 1, 10);
    assert(a == b);
    b.words[30] = 11;
    renderer::canonicalize_exact_pipeline(b, 1, 1, 10);
    assert(a != b); // relevant vertex texture hint must distinguish shaders
    b = a;
    b.words[8] = 0x800;
    b.words[16] = 3;
    renderer::canonicalize_exact_pipeline(b, 1, 1, 10);
    assert(b.words[16] == 3); // two-sided stencil remains relevant
    assert(a != b);
    b = a;
    b.words[3] = 1; // casts remain relevant
    assert(a != b);
    b = a;
    b.words[27] = 1; // gamma remains relevant
    assert(a != b);
}
