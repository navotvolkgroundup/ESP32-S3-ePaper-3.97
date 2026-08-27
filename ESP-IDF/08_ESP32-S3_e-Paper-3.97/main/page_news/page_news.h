#ifndef _PAGE_NEWS_H_
#define _PAGE_NEWS_H_

#ifdef __cplusplus
extern "C" {
#endif

// Fetches the Techmeme RSS feed and renders the headlines as a newspaper page.
// Blocks until the user double-clicks Function/Boot to return to the menu.
void page_news_show(void);

#ifdef __cplusplus
}
#endif

#endif
