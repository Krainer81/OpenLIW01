# Legal / publication notes

**Not legal advice.** This is a conservative publication policy for the project, not a legal opinion.

## Why the public repository is intentionally narrow

The public package distributes the independently written replacement firmware and the functional interface information needed to operate it. It excludes the original vendor firmware, decompiled/disassembled expression, PCB imagery/artwork and private evidence archives.

## EU / Czech software-copyright background

Directive 2009/24/EC on the legal protection of computer programs states that program **expression** is protected, while ideas and principles underlying a program, including those underlying interfaces, are not protected by copyright (Article 1(2)). Article 5(3) gives a person entitled to use a copy a right, under its conditions, to observe, study or test its functioning to determine underlying ideas/principles. Article 6 provides a narrower decompilation exception when indispensable to obtain information necessary for interoperability of an independently created program, subject to conditions and limits on how the information is used or shared.

Czech Act No. 121/2000 Coll. contains corresponding rules in §65–66. In particular, §66(1)(d) addresses study/testing of functionality by an authorized user, and §66(1)(e) addresses code reproduction/translation necessary for interoperability under stated conditions. §66(4) limits use/disclosure of information obtained under the interoperability provision.

References:
- EUR-Lex, Directive 2009/24/EC: https://eur-lex.europa.eu/legal-content/EN/TXT/?uri=CELEX:32009L0024
- Czech Copyright Act No. 121/2000 Coll., §65–66: https://www.zakonyprolidi.cz/cs/2000-121

These provisions do **not** answer every possible issue. Contract terms, trademarks, patents, trade-secret rules, unfair competition, device access controls/anti-circumvention rules and the facts of acquisition/use can matter.

## GitHub-specific risk management

GitHub operates copyright/DMCA notice-and-takedown processes and separately reviews anti-circumvention claims. A public repository can therefore still receive a complaint even where the author believes the work is lawful.

References:
- GitHub DMCA policy: https://docs.github.com/en/site-policy/content-removal-policies/dmca-takedown-policy
- GitHub content removal requests: https://docs.github.com/en/site-policy/content-removal-policies/submitting-content-removal-requests

## Recommended publication posture

Publish:
- independently authored YAML/C++/JS/CSS;
- interface facts needed to run the independent implementation;
- concise validation summaries;
- installation and safety documentation.

Do not publish by default:
- vendor `.bin` images or dumps;
- disassembly/decompilation listings or copied vendor source/expression;
- vendor web assets/logos/marketing images;
- PCB photos, Gerbers or reconstructed manufacturing artwork unless rights are separately cleared;
- credentials or private evidence archives.

Use the product/manufacturer names only descriptively and include an unaffiliated/trademark notice.
