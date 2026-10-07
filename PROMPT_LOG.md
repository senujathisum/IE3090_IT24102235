# Prompt Log – AI Interaction Record (IE3090 Assignment, Part 1)

**Student:** IT24102235
**AI usage level:** CLEAR Level 3 – AI Collaboration (allowed for Part 1 only)
**Tool used:** Claude Code (Anthropic, model Claude Opus 5.5), in the Claude desktop app
**Date of all interactions:** 2026-10-07

| # | Stage | What I asked for | How I used the output |
|---|---|---|---|
| 1 | Implementation | A complete reference implementation from the brief and Lab Sheets 1–9 | Retyped the three source files myself on CentOS, then built and tested them |
| 2 | Environment set-up | Step-by-step help for CentOS Stream 10 in VMware, SSH keys for GitHub, and git | Followed the steps; generated my own SSH key and created the repository |
| 3 | Testing | A test checklist and an automated test script | Ran the checklist manually on CentOS and took screenshots; kept the script as `tests/protocol_test.py` |
| 4 | Documentation | Drafts of README, design diary, prompt log, reflection and the report structure | Reviewed and edited them; added my own results and screenshots |
| 5 | Git | Meaningful commit messages | Used them for my commits |


## Interaction 1 – Reference implementation

**Prompt (summary):** I gave the AI the assignment brief and Lab Sheets 1–9 and asked it to complete the assignment for my registration number IT24102235, staying within what the labs teach.

**Output:**
- the personalised values: port 9410, SID:5322, token OPS-2235, `*_235` file names, log name and storage path;
- reference versions of `agent_235.c`, `controller_235.c` and `Makefile_235`, which it tested in Ubuntu (WSL);
- a test script, document drafts and a report draft.

**How I used it:**
- I did not submit the AI files directly.
- I created a new repository in my CentOS Stream 10 VM and **typed the three source files myself**, using the AI version as my reference.
- I built them with `make -f Makefile_235`. The build had no warnings.
- I tested every command by hand: AUTH, SYSINFO, LISTPROC, EXEC, PUT/GET with `md5sum`, MONITOR, several controllers at once, and a Ctrl+C disconnect.

## Interaction 2 – CentOS, GitHub and git set-up

**Prompt (summary):** How to do the whole assignment in my CentOS VM from start to finish, including uploading to GitHub from inside CentOS.

**Output:** steps for installing gcc/make/git, adding an SSH key to GitHub (`ssh-keygen`, as in Lab 6), creating the repository, adding a `.gitignore`, and committing and pushing.

**How I used it:** I followed the steps. I generated my own key pair and added only the **public** key to GitHub.

## Interaction 3 – Testing

**Output:**
- a manual test checklist with the expected response for each command;
- `tests/protocol_test.py`, an automated test script with 56 checks.

**How I used it:** I ran the checklist on CentOS and took the report screenshots during that run. All 56 test passed.

## Interaction 4 – Documentation

**Output:** drafts of `README.md`, `DESIGN_DIARY.md`, this prompt log, `REFLECTION.md`, and the Implementation Report structure (sections, architecture diagram, testing table, screenshot placeholders).

**How I used it:** I checked them against my own code and results. I replaced the placeholders with my own screenshots and wrote my own reflection.

## Interaction 5 – Commit messages

**Output:** suggested descriptive commit messages for each step.

**How I used it:** I used them for my commits on GitHub.
