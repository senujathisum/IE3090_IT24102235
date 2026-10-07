# Structured Reflection – IE3090 Assignment Part 1 (IT24102235)


**1. Which AI tools did I use, and at which stages?**

I used Anthropic's Claude Opus 5.5 on 2nd October 2026. First, I gave it the assignment brief and the lab sheets, and it produced a reference implementation of the Agent and Controller. I then built my own repository in my CentOS Stream 10 VM and typed the three source files myself, using that reference as a guide. I also used the AI for:
- the CentOS and GitHub set-up steps;
- a test checklist;
- drafts of the README, design diary, prompt log and report;
- clearer commit messages and to be organized.

**2. What did the AI do well? Where did it get things wrong or mislead me?**

It calculated my personalised values correctly (port 9410, SID:5322, token OPS-2235). It also stayed within the techniques from the labs: sockets, `pthread_create`, `select()` timeouts and `popen()`.

It was not perfect. In its own testing it found that its first version had used `inet_ntoa()`, which is not thread-safe. Its Controller could also loop forever at the end of input. Both had to be fixed. The sample log it produced came from its own Ubuntu test machine, not from mine, so I replaced it with a log from my CentOS run.


**3. What did I change, add, or reject from the AI output, and why?**

I did not submit the AI files directly. I created a new repository and typed every source file myself, so I had to read each function. I tested every command on CentOS myself, including checking with `md5sum` that downloaded files were byte-for-byte identical. I added a `.gitignore` after the binaries showed up in `git status`.


**4. What did I learn about my own understanding of network programming?**

- Typing `read_line()` showed you why TCP's byte stream needs a framing layer, and why one `recv()` is not one message 
- Why a thread per client was simpler than `fork()` once MONITOR needed a second task per session.
- The difference between the reliable TCP control channel and the UDP monitor stream 

