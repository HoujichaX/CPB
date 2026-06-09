# Security Considerations

## Risk Assessment

CPB combines dictionary-based compression, dictionary training, data visualization, and multi-dimensional data rearrangement. While designed for compression research and practical archival use, we recognize that some capabilities could potentially be repurposed.

This document describes the identified risks and the measures taken to address them.

### Identified Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| Data obfuscation via L4 shuffle | Medium | L4 is scrambling, not encryption. No key exchange, no cryptographic guarantees. |
| Evasion of inspection tools | Medium | Output uses standard AVI/ZIP/MP4 containers — any forensic tool can open them. |
| Dictionary rotation for tracking evasion | Low | Dictionaries are static files, not runtime-generated. Rotation requires manual effort. |
| High parameter freedom | Low | Profiles (STANDARD, ARCHIVE, etc.) provide safe defaults. Custom mode is opt-in. |
| Automated workflows | Low | GUI-based, not scriptable by default. No daemon mode. |

### Measures Taken

**1. Separation of concerns**

Steganographic capabilities have been separated into a distinct project (CMF — Chocolate Muffin), which is **not publicly released**. CPB contains no steganographic embedding, no LSB manipulation, and no carrier-aware hiding. CPB is a compression and archival tool.

**2. Dual licensing**

The CPB core (compression pipeline, RS codes, container formats) is released under Apache 2.0. Trained dictionaries and dictionary training tools are available under commercial license. This limits the availability of highly optimized dictionaries that could enable extreme compression for obfuscation purposes.

**3. Standard output formats**

CPB outputs standard file formats (AVI, ZIP, MP4, PDF, PNG) that are fully inspectable by any forensic tool. There is no proprietary container that resists analysis.

**4. No cryptographic claims**

L4 multi-dimensional shuffle provides data rearrangement, not encryption. It should not be relied upon for confidentiality. For actual encryption, use established tools (AES, ChaCha20, etc.) before or after CPB processing.

**5. Open source transparency**

The full source code is publicly available for review. There are no hidden features, no backdoors, and no obfuscated code paths.

### What CPB is NOT

- CPB is **not** an encryption tool
- CPB is **not** a steganographic tool
- CPB is **not** designed to evade law enforcement
- CPB is **not** designed to hide illegal content

### Responsible Disclosure

If you discover a security issue or a potential for misuse that we haven't addressed, please open an issue or contact the maintainer directly. We take these concerns seriously.

### History

This project underwent a formal risk assessment during early development. The assessment identified potential dual-use concerns, which led to:

1. Separation of steganographic features into a separate, non-public project
2. Implementation of safe-default profiles
3. Adoption of dual licensing for dictionary tools
4. Publication of this security document

We believe that transparency about risks, combined with concrete mitigation measures, is more responsible than obscurity.

---

*Last updated: June 2026*
