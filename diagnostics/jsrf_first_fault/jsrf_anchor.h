/* See jsrf_anchor.c. Writes a human phrase naming only the anchor fields that
 * differ; empty when they agree. Never reads guest memory -- both words are
 * already sampled, and one of them came off disk. */
#ifndef JSRF_ANCHOR_H
#define JSRF_ANCHOR_H

void jsrf_pad_anchor_describe(unsigned long recorded, unsigned long live,
                              char *out, unsigned long out_size);

#endif
