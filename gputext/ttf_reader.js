class MyBinaryStream {
  constructor(data) {
    // DataView returns big-endian numbers by default, which is what TTF needs.
    this.data = new DataView(data);
    this.offset = 0;
  }
  readU8() { return this.data.getUint8(this.offset++); }
  readI8() { return this.data.getInt8(this.offset++); }
  readU16() {
    const result = this.data.getUint16(this.offset);
    this.offset += 2;
    return result;
  }
  readI16() {
    const result = this.data.getInt16(this.offset);
    this.offset += 2;
    return result;
  }
  readFixed2_14() { return readI16() / (1 << 14); }
  readU32() {
    const result = this.data.getUint32(this.offset);
    this.offset += 4;
    return result;
  }
  readI64() {
    const result = this.data.getBigInt64(this.offset);
    this.offset += 8;
    return result;
  }
  readDATETIME() {
    return new Date(
        Date.parse("1904-01-01T00:00Z") + Number(this.readI64()) * 1000);
  }
  readTag() {
    let tag = '';
    for (let i = 0; i < 4; ++i)
      tag += String.fromCharCode(this.readU8());
    return tag;
  }
}

const SFNT_VERSION_TTF = 0x00010000;
const SFNT_VERSION_TTF_APPLE = 0x74727565; // 'true'
const SFNT_VERSION_POSTSCRIPT_APPLE = 0x74797031; // 'typ1'
const SFNT_VERSION_OPENTYPE = 0x4F54544F; // 'OTTO'

// https://docs.microsoft.com/en-us/typography/opentype/spec/otff
// https://developer.apple.com/fonts/TrueType-Reference-Manual/RM06/Chap6.html
class TTFReader {
  constructor(data) {
    this.stream = new MyBinaryStream(data);
    this.readHeader();
    this.readTableHeaders();
    this.readCmap();
    this.readHead();
    this.readHhea();
    this.readLoca();
  }
  readHeader() {
    // Required headers for ttf files:
    // 'cmap', 'glyf', 'head', 'hhea', 'hmtx', 'loca', 'maxp', 'name', 'post'.
    // 'cmap' maps characters (nowadays, unicode code points) to glyph indices.
    // 'glyf' contains raw gyph data.
    // 'head' contains global font information, e.g. if 'loca' is 16- or 32-bit.
    // 'loca' maps glyph indices to glyph data offsets.
    // 'maxp' contains number of glyphs (and other limits).
    this.sfntVersion = this.stream.readU32();
    this.numTables = this.stream.readU16();
    this.searchRange = this.stream.readU16();
    this.entrySelector = this.stream.readU16();
    this.rangeShift = this.stream.readU16();
  }
  readTableHeaders() {
    this.tableHeaders = {};
    for (let i = 0; i < this.numTables; ++i) {
      const tableTag = this.stream.readTag();
      this.tableHeaders[tableTag] = {
        checksum: this.stream.readU32(),
        offset: this.stream.readU32(),
        length: this.stream.readU32(),
      };
    }
  }
  readCmap() {
    // https://docs.microsoft.com/en-us/typography/opentype/spec/cmap
    const cmapPos = this.tableHeaders['cmap']
    if (cmapPos === undefined)
      throw new Error('Missing required "cmap" header')
    this.stream.offset = cmapPos.offset;
    this.cmapVersion = this.stream.readU16();
    this.cmapNumTables = this.stream.readU16();

    this.cmapEntries = {};
    for (let i = 0; i < this.cmapNumTables; ++i) {
      const platformId = this.stream.readU16();
      const encodingId = this.stream.readU16();
      const subtableOffset = this.stream.readU32();
      (this.cmapEntries[platformId] ??= {})[encodingId] = subtableOffset;
    }

    const CMAP_PLATFORM_UNICODE = 0;
    const CMAP_PLATFORM_APPLE_LEGACY = 1;
    const CMAP_PLATFORM_ISO_LEGACY = 2;
    const CMAP_PLATFORM_MICROSOFT = 3;

    const CMAP_UNICODE_ENCODING_1_0_DEPRECATED = 0;
    const CMAP_UNICODE_ENCODING_1_1_DEPRECATED = 1;
    const CMAP_UNICODE_ENCODING_ISO_IEC_10646_DEPRECATED = 2;
    const CMAP_UNICODE_ENCODING_2_0_BMP = 3;
    const CMAP_UNICODE_ENCODING_2_0_FULL = 4;
    const CMAP_UNICODE_ENCODING_VARIATION_SEQUENCES = 5;
    const CMAP_UNICODE_ENCODING_FULL = 6;

    const unicodeEntries = this.cmapEntries[CMAP_PLATFORM_UNICODE];
    if (unicodeEntries !== undefined) {
      // FIXME: non-BMP tables should have precedence when present.
      const unicode20Bmp = unicodeEntries[CMAP_UNICODE_ENCODING_2_0_BMP];
      if (unicode20Bmp !== undefined) {
        log('using unicode BMP cmap (0, 3)\n');
        this.readCmapSubtable(cmapPos.offset + unicode20Bmp);
        return;
      }
    }

    throw new Error(
        `no implemented cmap type, cmap: ${JSON.stringify(this.cmapEntries)}`);
  }
  readCmapSubtable(offset) {
    this.stream.offset = offset;
    const subtableFormat = this.stream.readU16();
    const subtableLength = this.stream.readU16();

    // Only non-0 for CMAP_PLATFORM_APPLE_LEGACY.
    const subtableLanguage = this.stream.readU16();

    if (subtableFormat === 4) {
      this.readCmapSubtable4();
      return;
    }

    log(`cmap subtable format ${subtableFormat}\n`)
  }
  readCmapSubtable4() {
    const segCountX2 = this.stream.readU16();
    const segCount = Math.trunc(segCountX2 / 2);
    const searchRangeFromFile = this.stream.readU16();
    const entrySelectorFromFile = this.stream.readU16();
    const rangeShiftFromFile = this.stream.readU16();

    let endCode = Array(segCount);
    for (let i = 0; i < segCount; ++i)
      endCode[i] = this.stream.readU16();

    const reservedPad = this.stream.readU16();

    let startCode = Array(segCount);
    for (let i = 0; i < segCount; ++i)
      startCode[i] = this.stream.readU16();

    let idDelta = Array(segCount);
    for (let i = 0; i < segCount; ++i)
      idDelta[i] = this.stream.readI16();

    const idRangeOffsetsOffset = this.stream.offset;
    let idRangeOffsets = Array(segCount);
    for (let i = 0; i < segCount; ++i)
      idRangeOffsets[i] = this.stream.readU16();

    if (endCode[endCode.length - 1] != 0xffff) {
      throw new Error('invalid cmap subtable 4: endCode array has wrong end ' +
                      endCode[endCode.length - 1]);
    }

    this.glyphIndexForChar = function(c) {
      const code = c.codePointAt();
      if (code > 0xffff)
        return 0;

      let l = 0, r = endCode.length - 1;
      while (l < r) {
        let mid = l + Math.trunc((r - l) / 2);
        if (code <= endCode[mid]) {
          r = mid;
        } else {
          l = mid + 1;
        }
      }

      if (startCode[l] > code)
        return 0;

      if (idRangeOffsets[l] === 0)
        return (code + idDelta[l]) & 0xffff;

      // idRangeOffsets are relative to the idRangeOffsets entry.
      const glyphIdArrayIndex = (idRangeOffsetsOffset + l*2) +
                                 idRangeOffsets[l] +
                                 (code - startCode[l]) * 2;
      this.stream.offset = glyphIdArrayIndex;
      const glyphIndex = this.stream.readU16();
      if (glyphIndex === 0)
        return 0;
      return (glyphIndex + idDelta[l]) & 0xffff;
    }
  }
  readHead() {
    // https://docs.microsoft.com/en-us/typography/opentype/spec/head
    const headPos = this.tableHeaders['head']
    if (headPos === undefined)
      throw new Error('Missing required "head" header')
    this.stream.offset = headPos.offset;
    this.head = {
      majorVersion: this.stream.readU16(), // 1
      minorVersion: this.stream.readU16(), // 0
      fontRevision: this.stream.readU32(),
      checksumAdjustment: this.stream.readU32(),
      magicNumber: this.stream.readU32(), // 0x5F0F3CF5.
      flags: this.stream.readU16(),
      unitsPerEm: this.stream.readU16(),
      created: this.stream.readDATETIME(),
      modified: this.stream.readDATETIME(),
      xMin: this.stream.readI16(),
      yMin: this.stream.readI16(),
      xMax: this.stream.readI16(),
      yMax: this.stream.readI16(),
      macStyle: this.stream.readU16(),
      lowestRecPPEM: this.stream.readU16(),
      fontDirectionHint_Deprecated: this.stream.readI16(), // 2
      indexToLocFormat: this.stream.readI16(),
      glypDataFormat: this.stream.readI16(),
    };
  }
  readHhea() {
    // https://docs.microsoft.com/en-us/typography/opentype/spec/hhea
    const hheaPos = this.tableHeaders['hhea']
    if (hheaPos === undefined)
      throw new Error('Missing required "hhea" header')
    this.stream.offset = hheaPos.offset;
    this.hhea = {
      majorVersion: this.stream.readU16(), // 1
      minorVersion: this.stream.readU16(), // 0
      ascender: this.stream.readI16(),
      descender: this.stream.readI16(),
      lineGap: this.stream.readI16(),
      advanceWidthMax: this.stream.readU16(),
      minLeftSideBearing: this.stream.readI16(),
      minRightSideBearing: this.stream.readI16(),
      xMaxExtent: this.stream.readI16(),
      caretSlopeRise: this.stream.readI16(),
      caretSlopeRun: this.stream.readI16(),
      caretOffset: this.stream.readI16(),
      reserved0: this.stream.readI16(),
      reserved1: this.stream.readI16(),
      reserved2: this.stream.readI16(),
      reserved3: this.stream.readI16(),
      metricDataFormat: this.stream.readI16(),
      numberOfHMetrics: this.stream.readU16(),
    };
  }
  readLoca() {
    // https://docs.microsoft.com/en-us/typography/opentype/spec/loca
    const locaPos = this.tableHeaders['loca']
    if (locaPos === undefined)
      throw new Error('Missing required "loca" header')
    this.stream.offset = locaPos.offset;

    // u16 offsets store offsets divided by 2. u32 offsets store actual offsets.
    if (this.head.indexToLocFormat === 0) {
      const n = locaPos.length / 2;
      this.glyphOffsets = Array(n);
      for (let i = 0; i < n; ++i)
        this.glyphOffsets[i] = this.stream.readU16() * 2;
    } else if (this.head.indexToLocFormat === 1) {
      const n = locaPos.length / 4;
      this.glyphOffsets = Array(n);
      for (let i = 0; i < n; ++i)
        this.glyphOffsets[i] = this.stream.readU32();
    } else {
      throw new Error(`invalid loca format ${this.head.indexToLocFormat}`);
    }
    log(`${this.glyphOffsets.length} glyphs\n`);
  }
  glyphForChar(c) {
    return this.glyphForGlyphIndex(this.glyphIndexForChar(c));
  }
  glyphForGlyphIndex(index) {
    // https://docs.microsoft.com/en-us/typography/opentype/spec/loca
    // "In order to compute the length of the last glyph element,
    // there is an extra entry after the last valid index [...]
    // This also applies to any other characters without an outline,
    // such as the space character. If a glyph has no outline, then
    // loca[n] = loca[n+1]".
    const glyphOffset = this.glyphOffsets[index];
    const glyphSize = this.glyphOffsets[index + 1] - this.glyphOffsets[index];
    return this.glyphForGlyphOffsetAndSize(glyphOffset, glyphSize);
  }
  glyphForGlyphOffsetAndSize(glyphOffset, glyphSize) {
    if (glyphSize === 0) // Space character and such.
      return null;

    // FIXME: Might make sense to have a cache indexed by arguments.

    // https://docs.microsoft.com/en-us/typography/opentype/spec/glyf
    const glyfPos = this.tableHeaders['glyf']
    if (glyfPos === undefined)
      throw new Error('Missing required "glyf" header')
    this.stream.offset = glyfPos.offset + glyphOffset;

    const numberOfContours = this.stream.readI16();
    const xMin = this.stream.readI16();
    const yMin = this.stream.readI16();
    const xMax = this.stream.readI16();
    const yMax = this.stream.readI16();

    if (numberOfContours >= 0) {
      // Simple glyph.
      let endPtsOfContours = Array(numberOfContours);
      for (let i = 0; i < numberOfContours; ++i)
        endPtsOfContours[i] = this.stream.readU16();
      const numPoints = endPtsOfContours[endPtsOfContours.length - 1] + 1;

      const instructionLength = this.stream.readU16();
      let instructions = Array(instructionLength);
      for (let i = 0; i < instructionLength; ++i)
        instructions[i] = this.stream.readU8();

      const ON_CURVE_POINT = 1;
      const X_SHORT_VECTOR = 2;
      const Y_SHORT_VECTOR = 4;
      const REPEAT_FLAG = 8;
      const X_IS_SAME_OR_POSITIVE_X_SHORT_VECTOR = 0x10;
      const Y_IS_SAME_OR_POSITIVE_Y_SHORT_VECTOR = 0x20;
      const OVERLAP_SIMPLE = 0x40;
      const Reserved = 0x80;

      let flags = Array(numPoints);
      for (let i = 0; i < numPoints;) {
        const flag = this.stream.readU8();
        flags[i++] = flag;
        if (flag & REPEAT_FLAG) {
          const numRepetitions = this.stream.readU8();
          for (let j = 0; j < numRepetitions; ++j)
            flags[i++] = flag;
        }
      }

      let xCoords = Array(numPoints);
      let x = 0;
      for (let i = 0; i < numPoints; ++i) {
        const flag = flags[i];
        if (flag & X_SHORT_VECTOR) {
          if ((flag & X_IS_SAME_OR_POSITIVE_X_SHORT_VECTOR) === 0)
            x -= this.stream.readU8();
          else
            x += this.stream.readU8();
        } else if ((flag & X_IS_SAME_OR_POSITIVE_X_SHORT_VECTOR) === 0)
          x += this.stream.readI16();
        xCoords[i] = x;
      }

      let yCoords = Array(numPoints);
      let y = 0;
      for (let i = 0; i < numPoints; ++i) {
        const flag = flags[i];
        if (flag & Y_SHORT_VECTOR) {
          if ((flag & Y_IS_SAME_OR_POSITIVE_Y_SHORT_VECTOR) === 0)
            y -= this.stream.readU8();
          else
            y += this.stream.readU8();
        } else if ((flag & Y_IS_SAME_OR_POSITIVE_Y_SHORT_VECTOR) === 0)
          y += this.stream.readI16();
        yCoords[i] = y;
      }

      let contours = Array(numberOfContours);
      for (let i = 0, k = 0; i < numberOfContours; ++i) {
        const numPointsOnContour = endPtsOfContours[i] + 1 - k;
        let points = Array(numPointsOnContour);
        for (let j = 0; j < numPointsOnContour; ++j, ++k) {
          points[j] = {
            x: xCoords[k],
            y: yCoords[k],
            isOnCurve: (flags[k] & ON_CURVE_POINT) !== 0,
          };
        }
        contours[i] = points;
      }

      return { contours, xMin, yMin, xMax, yMax };
    }

    // Compound glyph.
    const ARG_1_AND_2_ARE_WORDS = 1;
    const ARGS_ARE_XY_VALUES = 2;
    const ROUND_XY_TO_GRID = 4;
    const WE_HAVE_A_SCALE = 8;
    const MORE_COMPONENTS = 0x20;
    const WE_HAVE_AN_X_AND_Y_SCALE = 0x40;
    const WE_HAVE_A_TWO_BY_TWO = 0x80;
    const WE_HAVE_INSTRUCTIONS = 0x100
    const USE_MY_METRICS = 0x200;
    const OVERLAP_COMPOUND = 0x400;
    const SCALED_COMPONENT_OFFSET = 0x800;
    const UNSCALED_COMPONENT_OFFSET = 0x1000;

    let flags;
    let glyphRefs = [];
    do {
      flags = this.stream.readU16();
      const componentGlyphIndex = this.stream.readU16();

      // The transformation code is explained better in Apple's docs:
      // https://developer.apple.com/fonts/TrueType-Reference-Manual/RM06/Chap6glyf.html
      let m00 = 1, m01 = 0, m02 = 0;
      let m10 = 0, m11 = 1, m12 = 0;

      // FIXME: Support this. The two args are indices into composite glyph
      // so far, and into the new glyph, and the points at those two indices
      // are supposed to match when this is not set.
      if ((flags & ARGS_ARE_XY_VALUES) === 0)
        throw new Error('Support for !ARGS_ARE_XY_VALUES not yet implemented');

      if (flags & ARG_1_AND_2_ARE_WORDS) {
        m02 = this.stream.readI16();
        m12 = this.stream.readI16();
      } else {
        m02 = this.stream.readI8();
        m12 = this.stream.readI8();
      }

      if (flags & WE_HAVE_A_SCALE) {
        m00 = m11 = this.stream.readFixed2_14();
      } else if (flags & WE_HAVE_AN_X_AND_Y_SCALE) {
        m00 = this.stream.readFixed2_14();
        m11 = this.stream.readFixed2_14();
      } else if (flags & WE_HAVE_A_TWO_BY_TWO) {
        m00 = this.stream.readFixed2_14();
        m01 = this.stream.readFixed2_14();
        m01 = this.stream.readFixed2_14();
        m11 = this.stream.readFixed2_14();
      }

      // FIXME: Implement.
      if (flags & SCALED_COMPONENT_OFFSET)
        throw new Error(
            'Support for SCALED_COMPONENT_OFFSET not yet implemented');

      glyphRefs.push({componentGlyphIndex, m00, m01, m02, m10, m11, m12});
    } while (flags & MORE_COMPONENTS);

    if (flags & WE_HAVE_INSTRUCTIONS) {
      const instructionLength = this.stream.readU16();
      let instructions = Array(instructionLength);
      for (let i = 0; i < instructionLength; ++i)
        instructions[i] = this.stream.readU8();
    }

    return { glyphRefs };
  }
  advanceWidthForChar(c) {
    return this.advanceWidthForGlyphIndex(this.glyphIndexForChar(c));
  }
  advanceWidthForGlyphIndex(glyphIndex) {
    // https://docs.microsoft.com/en-us/typography/opentype/spec/hmtx
    const hmtxPos = this.tableHeaders['hmtx']
    if (hmtxPos === undefined)
      throw new Error('Missing required "hmtx" header')

    glyphIndex = Math.min(this.hhea.numberOfHMetrics - 1, glyphIndex);
    this.stream.offset = hmtxPos.offset + 4 * glyphIndex;
    return this.stream.readU16();
  }
}

