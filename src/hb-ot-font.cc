/*
 * Copyright © 2011,2014  Google, Inc.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Google Author(s): Behdad Esfahbod, Roozbeh Pournader
 */

#include "hb-private.hh"

#include "hb-ot.h"

#include "hb-font-private.hh"

#include "hb-ot-cmap-table.hh"
#include "hb-ot-hhea-table.hh"
#include "hb-ot-hmtx-table.hh"


struct metrics_accel_t
{
  unsigned int num_metrics;
  unsigned int num_advances;
  const OT::_mtx *table;
  hb_blob_t *blob;

  inline void init (hb_font_t *font, hb_tag_t _hea_tag, hb_tag_t _mtx_tag)
  {
    this->num_metrics = font->face->get_num_glyphs ();

    hb_blob_t *_hea_blob = OT::Sanitizer<OT::_hea>::sanitize (font->face->reference_table (_hea_tag));
    const OT::_hea *_hea = OT::Sanitizer<OT::_hea>::lock_instance (_hea_blob);
    this->num_advances = _hea->numberOfLongMetrics;
    hb_blob_destroy (_hea_blob);

    this->blob = OT::Sanitizer<OT::_mtx>::sanitize (font->face->reference_table (_mtx_tag));
    if (unlikely (!this->num_advances ||
		  2 * (this->num_advances + this->num_metrics) < hb_blob_get_length (this->blob)))
    {
      hb_blob_destroy (this->blob);
      this->blob = hb_blob_get_empty ();
    }
    this->table = OT::Sanitizer<OT::_mtx>::lock_instance (this->blob);
  }

  inline void fini (void)
  {
    hb_blob_destroy (this->blob);
  }

  inline unsigned int get_advance (hb_codepoint_t glyph) const
  {
    if (unlikely (glyph >= this->num_metrics))
      return 0;

    if (glyph >= this->num_advances)
      glyph = this->num_advances - 1;

    return this->table->longMetric[glyph].advance;
  }
};

struct hb_ot_font_t
{
  unsigned int num_glyphs;

  metrics_accel_t h_metrics;
  metrics_accel_t v_metrics;

  const OT::CmapSubtable *cmap;
  const OT::CmapSubtable *cmap_uvs;
  hb_blob_t *cmap_blob;
};


static hb_ot_font_t *
_hb_ot_font_create (hb_font_t *font)
{
  hb_ot_font_t *ot_font = (hb_ot_font_t *) calloc (1, sizeof (hb_ot_font_t));

  if (unlikely (!ot_font))
    return NULL;

  ot_font->num_glyphs = font->face->get_num_glyphs ();

  ot_font->h_metrics.init (font, HB_OT_TAG_hhea, HB_OT_TAG_hmtx);
  /* TODO Can we do this lazily? */
  ot_font->v_metrics.init (font, HB_OT_TAG_vhea, HB_OT_TAG_vmtx);

  ot_font->cmap_blob = OT::Sanitizer<OT::cmap>::sanitize (font->face->reference_table (HB_OT_TAG_cmap));
  const OT::cmap *cmap = OT::Sanitizer<OT::cmap>::lock_instance (ot_font->cmap_blob);
  const OT::CmapSubtable *subtable = NULL;
  const OT::CmapSubtable *subtable_uvs = NULL;

  /* 32-bit subtables. */
  if (!subtable) subtable = cmap->find_subtable (3, 10);
  if (!subtable) subtable = cmap->find_subtable (0, 6);
  if (!subtable) subtable = cmap->find_subtable (0, 4);
  /* 16-bit subtables. */
  if (!subtable) subtable = cmap->find_subtable (3, 1);
  if (!subtable) subtable = cmap->find_subtable (0, 3);
  if (!subtable) subtable = cmap->find_subtable (0, 2);
  if (!subtable) subtable = cmap->find_subtable (0, 1);
  if (!subtable) subtable = cmap->find_subtable (0, 0);
  /* Meh. */
  if (!subtable) subtable = &OT::Null(OT::CmapSubtable);

  /* UVS subtable. */
  if (!subtable_uvs) subtable_uvs = cmap->find_subtable (0, 5);
  /* Meh. */
  if (!subtable_uvs) subtable_uvs = &OT::Null(OT::CmapSubtable);

  ot_font->cmap = subtable;
  ot_font->cmap_uvs = subtable_uvs;

  return ot_font;
}

static void
_hb_ot_font_destroy (hb_ot_font_t *ot_font)
{
  hb_blob_destroy (ot_font->cmap_blob);
  ot_font->h_metrics.fini ();
  ot_font->v_metrics.fini ();

  free (ot_font);
}


static hb_bool_t
hb_ot_get_glyph (hb_font_t *font HB_UNUSED,
		 void *font_data,
		 hb_codepoint_t unicode,
		 hb_codepoint_t variation_selector,
		 hb_codepoint_t *glyph,
		 void *user_data HB_UNUSED)

{
  const hb_ot_font_t *ot_font = (const hb_ot_font_t *) font_data;

  if (unlikely (variation_selector))
  {
    switch (ot_font->cmap_uvs->get_glyph_variant (unicode,
						  variation_selector,
						  glyph))
    {
      case OT::GLYPH_VARIANT_NOT_FOUND:		return false;
      case OT::GLYPH_VARIANT_FOUND:		return true;
      case OT::GLYPH_VARIANT_USE_DEFAULT:	break;
    }
  }

  return ot_font->cmap->get_glyph (unicode, glyph);
}

static hb_position_t
hb_ot_get_glyph_h_advance (hb_font_t *font HB_UNUSED,
			   void *font_data,
			   hb_codepoint_t glyph,
			   void *user_data HB_UNUSED)
{
  const hb_ot_font_t *ot_font = (const hb_ot_font_t *) font_data;
  return font->em_scale_x (ot_font->h_metrics.get_advance (glyph));
}

static hb_position_t
hb_ot_get_glyph_v_advance (hb_font_t *font HB_UNUSED,
			   void *font_data,
			   hb_codepoint_t glyph,
			   void *user_data HB_UNUSED)
{
  const hb_ot_font_t *ot_font = (const hb_ot_font_t *) font_data;
  return font->em_scale_y (-ot_font->v_metrics.get_advance (glyph));
}

static hb_bool_t
hb_ot_get_glyph_h_origin (hb_font_t *font HB_UNUSED,
			  void *font_data HB_UNUSED,
			  hb_codepoint_t glyph HB_UNUSED,
			  hb_position_t *x HB_UNUSED,
			  hb_position_t *y HB_UNUSED,
			  void *user_data HB_UNUSED)
{
  /* We always work in the horizontal coordinates. */
  return true;
}

static hb_bool_t
hb_ot_get_glyph_v_origin (hb_font_t *font HB_UNUSED,
			  void *font_data,
			  hb_codepoint_t glyph,
			  hb_position_t *x,
			  hb_position_t *y,
			  void *user_data HB_UNUSED)
{
  /* TODO */
  return false;
}

static hb_position_t
hb_ot_get_glyph_h_kerning (hb_font_t *font,
			   void *font_data,
			   hb_codepoint_t left_glyph,
			   hb_codepoint_t right_glyph,
			   void *user_data HB_UNUSED)
{
  /* TODO */
  return 0;
}

static hb_position_t
hb_ot_get_glyph_v_kerning (hb_font_t *font HB_UNUSED,
			   void *font_data HB_UNUSED,
			   hb_codepoint_t top_glyph HB_UNUSED,
			   hb_codepoint_t bottom_glyph HB_UNUSED,
			   void *user_data HB_UNUSED)
{
  return 0;
}

static hb_bool_t
hb_ot_get_glyph_extents (hb_font_t *font HB_UNUSED,
			 void *font_data,
			 hb_codepoint_t glyph,
			 hb_glyph_extents_t *extents,
			 void *user_data HB_UNUSED)
{
  /* TODO */
  return false;
}

static hb_bool_t
hb_ot_get_glyph_contour_point (hb_font_t *font HB_UNUSED,
			       void *font_data,
			       hb_codepoint_t glyph,
			       unsigned int point_index,
			       hb_position_t *x,
			       hb_position_t *y,
			       void *user_data HB_UNUSED)
{
  /* TODO */
  return false;
}

static hb_bool_t
hb_ot_get_glyph_name (hb_font_t *font HB_UNUSED,
		      void *font_data,
		      hb_codepoint_t glyph,
		      char *name, unsigned int size,
		      void *user_data HB_UNUSED)
{
  /* TODO */
  return false;
}

static hb_bool_t
hb_ot_get_glyph_from_name (hb_font_t *font HB_UNUSED,
			   void *font_data,
			   const char *name, int len, /* -1 means nul-terminated */
			   hb_codepoint_t *glyph,
			   void *user_data HB_UNUSED)
{
  /* TODO */
  return false;
}


static hb_font_funcs_t *
_hb_ot_get_font_funcs (void)
{
  static const hb_font_funcs_t ot_ffuncs = {
    HB_OBJECT_HEADER_STATIC,

    true, /* immutable */

    {
#define HB_FONT_FUNC_IMPLEMENT(name) hb_ot_get_##name,
      HB_FONT_FUNCS_IMPLEMENT_CALLBACKS
#undef HB_FONT_FUNC_IMPLEMENT
    }
  };

  return const_cast<hb_font_funcs_t *> (&ot_ffuncs);
}


void
hb_ot_font_set_funcs (hb_font_t *font)
{
  hb_ot_font_t *ot_font = _hb_ot_font_create (font);
  if (unlikely (!ot_font))
    return;

  hb_font_set_funcs (font,
		     _hb_ot_get_font_funcs (),
		     ot_font,
		     (hb_destroy_func_t) _hb_ot_font_destroy);
}
