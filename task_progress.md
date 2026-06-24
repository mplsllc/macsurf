# Crash Investigation & Fix Todo List

## Open Questions Requiring Source Reads

- [ ] **Q1**: Read llcache_object_notify_users and llcache_catch_up_all_users in full. Confirm reentrancy guard status. Write reentrancy guard fix for Crash D.
- [ ] **Q2**: Read hlcache_clean in full. Confirm whether entries are unlinked from content_list before free(entry). Fix Crash E.
- [ ] **Q3**: Read html_close in full. Find every guit->misc->schedule call in html.c and box_construct.c. Confirm cancellations. Fix Crash H.
- [ ] **Q4**: Read fetch_https.c teardown sequence. Confirm OTCloseProvider/nsurl_unref/free order. Fix Crash I.
- [ ] **Q5**: Read CSS budget accumulator. Find increment and reset points. Fix budget accumulation across navigations.
- [ ] **Q6**: Read connection pool implementation. Find max age enforcement. Fix connection pool age issue.
- [ ] **Q7**: Confirm Crash G (favicon.current dangling) fix needed - read browser_window.c lines 452-455.
- [ ] **Q8**: Confirm retry cap exists independently of dead-host preload for jsdelivr loop.
