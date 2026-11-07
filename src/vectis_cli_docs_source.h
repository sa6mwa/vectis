#ifndef VECTIS_CLI_DOCS_SOURCE_H
#define VECTIS_CLI_DOCS_SOURCE_H

#include <libmdf.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct vectis_cli_embedded_resource {
  const char *path;
  const char *module;
  const char *origin;
  const char *sha256;
  size_t offset;
  size_t size;
} vectis_cli_embedded_resource;

/* Borrow immutable resources. Only the current heading needs scratch space. */
typedef struct vectis_docs_pager_source {
  const vectis_cli_embedded_resource *documents;
  const unsigned char *data;
  size_t count;
  size_t document_index;
  size_t section_offset;
  size_t heading_size;
  int section;
  char heading[PATH_MAX];
} vectis_docs_pager_source;

static int vectis_docs_pager_section(vectis_docs_pager_source *source,
                                     const char **data, size_t *size) {
  const vectis_cli_embedded_resource *document;
  int length;

  document = &source->documents[source->document_index];
  if (source->section == 0) {
    if (source->heading_size == 0u) {
      length = snprintf(source->heading, sizeof(source->heading),
                        "# Vectis document: `%s`\n\n**SHA-256:** `%s`\n\n",
                        document->path, document->sha256);
      if (length < 0 || (size_t)length >= sizeof(source->heading))
        return -1;
      source->heading_size = (size_t)length;
    }
    *data = source->heading;
    *size = source->heading_size;
  } else if (source->section == 1) {
    *data = (const char *)source->data + document->offset;
    *size = document->size;
  } else {
    *data = "\n\n";
    *size = 2u;
  }
  return 0;
}

static void vectis_docs_pager_next(vectis_docs_pager_source *source) {
  source->section_offset = 0u;
  if (++source->section == 3) {
    source->document_index++;
    source->section = 0;
    source->heading_size = 0u;
  }
}

static size_t vectis_docs_pager_read(void *userdata, char *dst, size_t cap,
                                     int *err) {
  vectis_docs_pager_source *source;
  const char *data;
  size_t size, copied, written;

  source = (vectis_docs_pager_source *)userdata;
  written = 0u;
  if (err != NULL)
    *err = 0;
  if (source == NULL || dst == NULL) {
    if (err != NULL)
      *err = 1;
    return 0u;
  }
  while (written < cap && source->document_index < source->count) {
    if (vectis_docs_pager_section(source, &data, &size) != 0) {
      if (err != NULL)
        *err = 1;
      return 0u;
    }
    copied = size - source->section_offset;
    if (copied > cap - written)
      copied = cap - written;
    memcpy(dst + written, data + source->section_offset, copied);
    source->section_offset += copied;
    written += copied;
    if (source->section_offset == size)
      vectis_docs_pager_next(source);
  }
  return written;
}

static int vectis_docs_pager_seek(void *userdata,
                                  const mdf_source_offset *offset) {
  vectis_docs_pager_source *source, cursor;
  const char *data;
  size_t remaining, size;

  source = (vectis_docs_pager_source *)userdata;
  if (source == NULL || offset == NULL || offset->high > 0xffffffffUL ||
      offset->low > 0xffffffffUL || offset->high > ((size_t)-1 >> 16 >> 16) ||
      (unsigned long)(size_t)offset->low != offset->low)
    return -1;
  remaining = (size_t)offset->high * 65536u * 65536u + (size_t)offset->low;
  cursor = *source;
  cursor.document_index = 0u;
  cursor.section_offset = 0u;
  cursor.heading_size = 0u;
  cursor.section = 0;
  while (cursor.document_index < cursor.count) {
    if (vectis_docs_pager_section(&cursor, &data, &size) != 0)
      return -1;
    if (remaining < size) {
      cursor.section_offset = remaining;
      *source = cursor;
      return 0;
    }
    remaining -= size;
    vectis_docs_pager_next(&cursor);
  }
  if (remaining != 0u)
    return -1;
  *source = cursor;
  return 0;
}

#endif
