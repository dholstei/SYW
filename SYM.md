# SYW - Show Your Work

## Overview

SYW (Show Your Work) is a lightweight system for recording, preserving, and reviewing the evolution of thought.  Rather than evaluating only a final document, SYW captures the intellectual process that created it. Drafts, revisions, alternate ideas, branching lines of reasoning, source additions, corrections, and refinements become part of the record.

SYW is built on a simple observation:

> In most disciplines, the process that produced the answer is often more valuable than the answer itself.

Mathematics has long embraced this philosophy through derivations and partial credit. Software development embraces it through version control. Scientific research relies on laboratory notebooks, revision histories, and peer review.  SYW aims to bring the same philosophy to every form of intellectual work.

---

# The Problem

For centuries, educators, reviewers, editors, and employers have been forced to evaluate artifacts rather than processes.

Examples include:

- Essays
- Research papers
- Business plans
- Lab reports
- Design documents
- Theses
- Programming assignments

The reviewer typically sees a final document and must infer how it was created.  This creates numerous problems:

- Plagiarism detection becomes difficult.
- AI usage becomes difficult to evaluate.
- Genuine intellectual growth remains invisible.
- Dead ends, exploration, and learning are lost.
- Students receive feedback only after work is complete.
- Reviewers cannot distinguish ignorance from considered rejection.

The problem did not begin with AI; students copied lab reports before personal computers existed, authors plagiarized before the internet, and researchers borrowed ideas long before search engines.  AI simply exposed weaknesses that have existed for generations.

---

# The Core Idea

SYW records the evolution of thought, a final document becomes only one view of a larger process.

Instead of:

```text
Assignment
    ↓
Final Submission
```

SYW creates:

```text
Assignment
    ↓
Outline
    ↓
Research
    ↓
Revisions
    ↓
Alternative Ideas
    ↓
Branches
    ↓
Feedback
    ↓
Final Submission
```

The history itself becomes a valuable artifact.

---

# Guiding Principles

## Show Process, Not Just Results

A good answer is useful though a good process is educational.

SYW captures:

- Notes
- Drafts
- Revisions
- Deleted ideas
- Alternative approaches
- Sources
- Feedback
- Reasoning

---

## Make Thinking Visible

Traditional documents show conclusions, SYW shows decisions.

Examples include:

- Why a source was chosen
- Why a source was rejected
- How an argument evolved
- When a conclusion changed
- Which alternatives were considered

---

## Preserve Exploration

Many valuable ideas are discarded before reaching a final draft and, historically, preserving such ideas has been difficult.  Version control makes preservation effectively free.

Instead of deleting ideas:

```text
Delete
```

SYW encourages:

```text
Branch
```

Ideas can be explored, abandoned, revisited, merged, or recovered later.

---

## Encourage Reflection

Each revision becomes an opportunity for the author to describe:

- What changed
- Why it changed
- What was learned

The act of recording a revision encourages metacognition and self-reflection.

---

# Educational Benefits

## For Students

Students receive a record of their own intellectual growth.

They can:

- Review previous thinking
- Recover abandoned ideas
- Explore alternatives safely
- Learn from mistakes
- Demonstrate effort and understanding
- Document their reasoning process

Most importantly:

> Students gain credit for thinking, not merely for arriving at a polished result.

---

## For Educators

Educators gain insight previously unavailable.  Instead of seeing only a final paper, they can review:

- Revision history
- Sources added
- Changes in reasoning
- Alternative hypotheses
- Development over time

This allows:

- Better feedback
- Earlier intervention
- More accurate assessment
- Recognition of partial understanding
- Recognition of intellectual growth

A teacher may discover:

> The final answer is incorrect, but the reasoning process is excellent.

That distinction often matters.

---

## For Research Advisors

Graduate advisors and supervisors can:

- Monitor progress
- See how ideas evolve
- Review alternate approaches
- Understand abandoned directions
- Provide guidance during development

Rather than seeing only results, advisors gain visibility into the research journey.

---

# AI and SYW

SYW is not an anti-AI system, it assumes AI is here to stay.  Rather than attempting to prohibit AI, SYW places AI into context by becoming another source of input alongside:

- Books
- Journals
- Tutors
- Peers
- Professors
- Search engines

The important question becomes:

> How did the author evaluate and use the information?

A revision history can demonstrate:

- AI suggestions accepted
- AI suggestions rejected
- Claims verified
- Sources added
- Reasoning refined

The focus shifts from:

```text
Was AI used?
```

to:

```text
Was judgment exercised?
```

---

# Plagiarism and Academic Integrity

SYW cannot eliminate plagiarism, no system can, however, SYW changes the environment in which plagiarism occurs.

A document accompanied by:

- Commit history
- Revision history
- Development timeline
- Alternative branches

contains significantly more evidence of authorship than a final document alone.  Rather than relying solely on detection after the fact, SYW promotes transparency throughout the creation process.

---

# Publishing and Professional Writing

The benefits extend beyond education, authors, editors, researchers, and publishers can all benefit from preserved revision histories.

Potential applications include:

- Books
- Articles
- Whitepapers
- Reports
- Journalism
- Grant proposals
- Technical documentation

A revision history can reveal:

- Research evolution
- Editorial decisions
- Alternative structures
- Rejected ideas
- Source development

Future scholars may value these histories as much as the final work itself.

---

# Branches: Preserving Alternate Lines of Thought

One of the most powerful capabilities inherited from version control is branching, which allow authors to explore ideas without risking existing work.

Example:

```text
Main Thesis
    |
    +-- Economic Interpretation
    |
    +-- Political Interpretation
    |
    +-- Cultural Interpretation
```

The final paper may contain only one path, however, the record demonstrates that alternatives were explored.  This creates evidence of consideration rather than omission, branches become receipts for intellectual exploration.

---

# Technology

SYW intentionally builds on proven technologies, rather than inventing a new version control system, SYW leverages:

- Git
- Markdown
- Pandoc
- Existing document editors
- Existing hosting solutions

The goal is to provide a higher-level experience while relying on mature underlying tools.

---

# Initial Implementation

The first implementation focuses on simplicity.

Core workflow:

```text
Document
    ↓
Pandoc
    ↓
Markdown
    ↓
Git
    ↓
Commit
    ↓
Push
```

Initial implementation goals:

- Repository creation
- Document monitoring
- Markdown export
- Git commits
- Git pushes
- Commit message interface
- Diff visualization
- Timeline visualization

A Qt-based C++ application provides the user experience while Git and Pandoc provide the underlying infrastructure.

---

# Long-Term Vision

SYW aims to become a universal "show your work" platform.  Its purpose is not merely to combat AI, plagiarism, or academic dishonesty.
Its purpose is to preserve and encourage intellectual development, the final artifact tells us what was produced.
The history tells us how someone learned, reasoned, explored, revised, and ultimately arrived there.
That history is often the most valuable part of the work.

---
