import zipfile
import xml.sax.saxutils as saxutils
import os

def create_docx(filename):
    # XML templates
    content_types = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
</Types>"""

    rels = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
</Relationships>"""

    doc_rels = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>"""

    styles = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:docDefaults>
    <w:rPrDefault>
      <w:rPr>
        <w:rFonts w:ascii="Segoe UI" w:hAnsi="Segoe UI" w:cs="Segoe UI"/>
        <w:sz w:val="22"/>
        <w:color w:val="1E293B"/>
      </w:rPr>
    </w:rPrDefault>
  </w:docDefaults>
</w:styles>"""

    # Read markdown walkthrough to populate text
    with open("DEMO_WALKTHROUGH.md", "r", encoding="utf-8") as f:
        md_content = f.read()

    doc_body = []
    
    def p(text, bold=False, color="1E293B", size=22, space_after=120, bg=None):
        shd = f'<w:shd w:val="clear" w:color="auto" w:fill="{bg}"/>' if bg else ""
        b = "<w:b/>" if bold else ""
        t = saxutils.escape(text)
        return f'<w:p><w:pPr><w:spacing w:after="{space_after}"/>{shd}</w:pPr><w:r><w:rPr>{b}<w:color w:val="{color}"/><w:sz w:val="{size}"/></w:rPr><w:t xml:space="preserve">{t}</w:t></w:r></w:p>'

    def heading1(text):
        return p(text, bold=True, color="0369A1", size=32, space_after=200)

    def heading2(text):
        return p(text, bold=True, color="0F172A", size=26, space_after=160)

    def heading3(text):
        return p(text, bold=True, color="0284C7", size=24, space_after=120)

    def code_box(code):
        t = saxutils.escape(code)
        return f'''<w:p><w:pPr>
            <w:pBdr>
              <w:top w:val="single" w:sz="6" w:space="4" w:color="0284C7"/>
              <w:left w:val="single" w:sz="6" w:space="4" w:color="0284C7"/>
              <w:bottom w:val="single" w:sz="6" w:space="4" w:color="0284C7"/>
              <w:right w:val="single" w:sz="6" w:space="4" w:color="0284C7"/>
            </w:pBdr>
            <w:shd w:val="clear" w:color="auto" w:fill="0F172A"/>
            <w:spacing w:before="100" w:after="100"/>
          </w:pPr>
          <w:r>
            <w:rPr>
              <w:rFonts w:ascii="Consolas" w:hAnsi="Consolas"/>
              <w:color w:val="38BDF8"/>
              <w:sz w:val="20"/>
            </w:rPr>
            <w:t xml:space="preserve">{t}</w:t>
          </w:r>
        </w:p>'''

    def callout(speaker_text):
        t = saxutils.escape(speaker_text)
        return f'''<w:p><w:pPr>
            <w:pBdr><w:left w:val="single" w:sz="24" w:space="8" w:color="16A34A"/></w:pBdr>
            <w:shd w:val="clear" w:color="auto" w:fill="F0FDF4"/>
            <w:spacing w:before="80" w:after="160"/>
          </w:pPr>
          <w:r>
            <w:rPr>
              <w:i/>
              <w:color w:val="166534"/>
              <w:sz w:val="21"/>
            </w:rPr>
            <w:t xml:space="preserve">{t}</w:t>
          </w:r>
        </w:p>'''

    doc_body.append(heading1("CopyFast — High-Throughput Linux Asynchronous Copy/Backup Engine"))
    doc_body.append(p("Comprehensive Peer Review Dossier, Technical Architecture & Live Presentation Script", bold=True, color="64748B", size=24, space_after=240))
    doc_body.append(p("Key Highlights: C11 CoreLinux · Linux io_uring SQ/CQ Rings · Overlapped Pthreads Pipeline (2.31x Speedup) · Sparse SEEK_HOLE Preservation (197 GB/s) · Crash-Resilient CRC32 State Journal · In-Flight Streaming SHA-256 · Valgrind Clean 0 Leaks · 12/12 Tests Passing", bold=True, color="0369A1", size=20, space_after=300))

    # Parse and structure the walkthrough lines
    lines = md_content.splitlines()
    in_code = False
    code_lines = []

    for line in lines:
        if line.startswith("# "):
            continue
        elif line.startswith("## "):
            doc_body.append(heading2(line[3:].strip()))
        elif line.startswith("### Command"):
            doc_body.append(heading3(line[4:].strip()))
        elif line.startswith("```"):
            if in_code:
                doc_body.append(code_box("\n".join(code_lines)))
                code_lines = []
                in_code = False
            else:
                in_code = True
        elif in_code:
            code_lines.append(line)
        elif line.startswith("> *\""):
            cleaned = line.replace("> *\"", "").replace("\"*", "").strip()
            doc_body.append(callout("Speaker Script: " + cleaned))
        elif line.startswith("- **Technical Explanation**:"):
            doc_body.append(p(line[2:], bold=False, color="334155", size=21, space_after=100))
        elif line.startswith("- **What it does"):
            doc_body.append(p(line[2:], bold=False, color="334155", size=21, space_after=100))
        elif line.strip() and not line.startswith("---") and not line.startswith("|"):
            doc_body.append(p(line.strip(), size=21, space_after=100))

    document_xml = f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:body>
    {''.join(doc_body)}
    <w:sectPr>
      <w:pgSz w:w="11906" w:h="16838"/>
      <w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440"/>
    </w:sectPr>
  </w:body>
</w:document>"""

    with zipfile.ZipFile(filename, "w", zipfile.ZIP_DEFLATED) as docx:
        docx.writestr("[Content_Types].xml", content_types)
        docx.writestr("_rels/.rels", rels)
        docx.writestr("word/_rels/document.xml.rels", doc_rels)
        docx.writestr("word/styles.xml", styles)
        docx.writestr("word/document.xml", document_xml)

    print(f"Generated {filename} successfully ({os.path.getsize(filename)} bytes)")

if __name__ == "__main__":
    create_docx("CopyFast_Peer_Review_Dossier.docx")
