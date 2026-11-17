// HTML / XML to plain text for the model: tags dropped, scripts and styles skipped, block tags become line breaks,
// entities decoded, white space collapsed. xml = 1 for the XML of office documents (paragraphs end lines).
#ifndef HTMLTEXT_H
#define HTMLTEXT_H

// Converts in[0..n) into out (NUL-terminated, at most max-1 bytes). Returns the length.
int html_to_text(const char* in, int n, char* out, int max, int xml);

#endif
