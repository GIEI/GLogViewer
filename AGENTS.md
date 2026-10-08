# Project language policy

All content written in this repository must be in English. This includes source code identifiers, every code comment, documentation, examples, test names, commit messages, issue and pull request text prepared for this project, and agent configuration files.

When editing existing content, translate any touched non-English prose or comments into English. Preserve externally defined tokens, protocol fields, file paths, and verbatim test fixtures when changing them would break behavior; explain such exceptions in English.

Apply this policy before saving files and verify it during review. The project-specific `english-project-guardian` agent in `.cursor/agents/` can audit changes for violations.
