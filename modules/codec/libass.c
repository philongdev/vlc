/*****************************************************************************
 * SSA/ASS subtitle decoder using libass.
 *****************************************************************************
 * Copyright (C) 2008-2009 VLC authors and VideoLAN
 *
 * Authors: Laurent Aimar <fenrir@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
#   include "config.h"
#endif

#include <string.h>
#include <limits.h>
#include <assert.h>
#include <math.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_codec.h>
#include <vlc_input.h>
#include <vlc_dialog.h>
#include <vlc_stream.h>

#include <ass/ass.h>

#if LIBASS_VERSION < 0x01300000
# define ASS_FONTPROVIDER_AUTODETECT 1
#endif

#if defined(_WIN32)
#   include <vlc_charset.h>
#endif

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Create ( vlc_object_t * );
static void Destroy( vlc_object_t * );

#define TEXT_SSA_FONTSDIR   N_("Additional fonts directory")

vlc_module_begin ()
    set_shortname( N_("Subtitles (advanced)"))
    set_description( N_("Subtitle renderers using libass") )
    set_capability( "spu decoder", 100 )
    set_subcategory( SUBCAT_INPUT_SCODEC )
    set_callbacks( Create, Destroy )
    add_string("ssa-fontsdir", NULL, TEXT_SSA_FONTSDIR, NULL)
vlc_module_end ()

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int DecodeBlock( decoder_t *, block_t * );
static void Flush( decoder_t * );

/* */
typedef struct
{
    vlc_tick_t     i_last_pts;
    vlc_tick_t     i_max_stop;

    /* The following fields of decoder_sys_t are shared between decoder and spu units */
    vlc_mutex_t    lock;
    int            i_refcount;

    /* */
    ASS_Library    *p_library;
    ASS_Renderer   *p_renderer;

    /* */
    ASS_Track      *p_track;
} decoder_sys_t;
static void DecSysRelease( decoder_sys_t *p_sys );
static void DecSysHold( decoder_sys_t *p_sys );

/* */
static void SubpictureUpdate( subpicture_t *, const struct vlc_spu_updater_configuration * );
static void SubpictureDestroy( subpicture_t * );

typedef struct
{
    decoder_sys_t *p_dec_sys;
    vlc_tick_t    i_pts;
} libass_spu_updater_sys_t;

typedef struct
{
    int x0;
    int y0;
    int x1;
    int y1;
} rectangle_t;

static int BuildRegions( rectangle_t *p_region, int i_max_region, ASS_Image *p_img_list, int i_width, int i_height );
static void RegionDraw( subpicture_region_t *p_region, ASS_Image *p_img );
static void OldEngineClunkyRollInfoPatch( decoder_t *p_dec, ASS_Track * );

//#define DEBUG_REGION

/*****************************************************************************
 * Create: Open libass decoder.
 *****************************************************************************/
static int Create( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t *)p_this;
    decoder_sys_t *p_sys;

    if( p_dec->fmt_in->i_codec != VLC_CODEC_SSA )
        return VLC_EGENERIC;

    p_dec->pf_decode = DecodeBlock;
    p_dec->pf_flush  = Flush;

    p_dec->p_sys = p_sys = malloc( sizeof( decoder_sys_t ) );
    if( !p_sys )
        return VLC_ENOMEM;

    /* */
    vlc_mutex_init( &p_sys->lock );
    p_sys->i_refcount = 1;
    p_sys->i_last_pts = VLC_TICK_INVALID;
    p_sys->i_max_stop = VLC_TICK_INVALID;
    p_sys->p_library  = NULL;
    p_sys->p_renderer = NULL;
    p_sys->p_track    = NULL;

    /* Create libass library */
    ASS_Library *p_library = p_sys->p_library = ass_library_init();
    if( !p_library )
    {
        msg_Warn( p_dec, "Libass library creation failed" );
        DecSysRelease( p_sys );
        return VLC_EGENERIC;
    }

    /* load attachments */
    input_attachment_t  **pp_attachments;
    int                   i_attachments;
    if( decoder_GetInputAttachments( p_dec, &pp_attachments, &i_attachments ))
    {
        i_attachments = 0;
        pp_attachments = NULL;
    }
    for( int k = 0; k < i_attachments; k++ )
    {
        input_attachment_t *p_attach = pp_attachments[k];

        bool found = false;

        /* Check mimetype*/
        if( !strcasecmp( p_attach->psz_mime, "application/x-truetype-font" ) )
            found = true;
        /* Then extension */
        else if( !found && strnlen( p_attach->psz_name, 4+1 ) > 4 )
        {
            char *ext = p_attach->psz_name + strlen( p_attach->psz_name ) - 4;

            if( !strcasecmp( ext, ".ttf" ) || !strcasecmp( ext, ".otf" ) || !strcasecmp( ext, ".ttc" ) )
                found = true;
        }

        if( found )
        {
            msg_Dbg( p_dec, "adding embedded font %s", p_attach->psz_name );

            ass_add_font( p_sys->p_library, p_attach->psz_name, p_attach->p_data, p_attach->i_data );
        }
        vlc_input_attachment_Release( p_attach );
    }
    free( pp_attachments );

    char *psz_fontsdir = var_InheritString( p_dec, "ssa-fontsdir" );
    if( psz_fontsdir )
    {
        ass_set_fonts_dir( p_library, psz_fontsdir );
        free( psz_fontsdir );
    }

    ass_set_extract_fonts( p_library, true );
    ass_set_style_overrides( p_library, NULL );

    /* Create the renderer */
    ASS_Renderer *p_renderer = p_sys->p_renderer = ass_renderer_init( p_library );
    if( !p_renderer )
    {
        msg_Warn( p_dec, "Libass renderer creation failed" );
        DecSysRelease( p_sys );
        return VLC_EGENERIC;
    }

    ass_set_use_margins( p_renderer, false);
    //if( false )
    //    ass_set_margins( p_renderer, int t, int b, int l, int r);
    ass_set_font_scale( p_renderer, 1.0 );
    ass_set_line_spacing( p_renderer, 0.0 );

#if defined( __ANDROID__ )
    const char *psz_font, *psz_family;
    const char *psz_font_droid = "/system/fonts/DroidSans-Bold.ttf";
    const char *psz_family_droid = "Droid Sans Bold";
    const char *psz_font_noto = "/system/fonts/NotoSansCJK-Regular.ttc";
    const char *psz_family_noto = "Noto Sans";

    // Workaround for Android 5.0+, since libass doesn't parse the XML yet
    if( access( psz_font_noto, R_OK ) != -1 )
    {
        psz_font = psz_font_noto;
        psz_family = psz_family_noto;
    }
    else
    {
        psz_font = psz_font_droid;
        psz_family = psz_family_droid;
    }

#elif defined( __APPLE__ )
    const char *psz_font = NULL; /* We don't ship a default font with VLC */
    const char *psz_family = "Helvetica Neue"; /* Use HN if we can't find anything more suitable - Arial is not on all Apple platforms */
#else
    const char *psz_font = NULL; /* We don't ship a default font with VLC */
    const char *psz_family = "Arial"; /* Use Arial if we can't find anything more suitable */
#endif

#ifdef HAVE_FONTCONFIG
#if defined(_WIN32)
    vlc_dialog_id *p_dialog_id =
        vlc_dialog_display_progress( p_dec, true, 0.0, NULL,
                                    _("Building font cache"),
                                    _( "Please wait while your font cache is rebuilt.\n"
                                    "This should take less than a minute." ) );
#endif
    ass_set_fonts( p_renderer, psz_font, psz_family, ASS_FONTPROVIDER_AUTODETECT, NULL, 1 );  // setup default font/family
#if defined(_WIN32)
    if( p_dialog_id != 0 )
        vlc_dialog_release( p_dec, p_dialog_id );
#endif
#else
    ass_set_fonts( p_renderer, psz_font, psz_family, ASS_FONTPROVIDER_AUTODETECT, NULL, 0 );
#endif

    /* Anything else than NONE will break smooth img updating.
       TODO: List and force ASS_HINTING_LIGHT for known problematic fonts */
    ass_set_hinting( p_renderer, ASS_HINTING_NONE );

    /* Add a track */
    ASS_Track *p_track = p_sys->p_track = ass_new_track( p_sys->p_library );
    if( !p_track )
    {
        DecSysRelease( p_sys );
        return VLC_EGENERIC;
    }
    ass_process_codec_private( p_track, p_dec->fmt_in->p_extra, p_dec->fmt_in->i_extra );
    OldEngineClunkyRollInfoPatch( p_dec, p_track );

    p_dec->fmt_out.i_codec = VLC_CODEC_RGBA;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Destroy: finish
 *****************************************************************************/
static void Destroy( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t *)p_this;

    DecSysRelease( p_dec->p_sys );
}

static void DecSysHold( decoder_sys_t *p_sys )
{
    vlc_mutex_lock( &p_sys->lock );
    p_sys->i_refcount++;
    vlc_mutex_unlock( &p_sys->lock );
}
static void DecSysRelease( decoder_sys_t *p_sys )
{
    /* */
    vlc_mutex_lock( &p_sys->lock );
    p_sys->i_refcount--;
    if( p_sys->i_refcount > 0 )
    {
        vlc_mutex_unlock( &p_sys->lock );
        return;
    }
    vlc_mutex_unlock( &p_sys->lock );

    if( p_sys->p_track )
        ass_free_track( p_sys->p_track );
    if( p_sys->p_renderer )
        ass_renderer_done( p_sys->p_renderer );
    if( p_sys->p_library )
        ass_library_done( p_sys->p_library );

    free( p_sys );
}

/*****************************************************************************
 * Flush:
 *****************************************************************************/
static void Flush( decoder_t *p_dec )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    p_sys->i_max_stop = VLC_TICK_INVALID;
    p_sys->i_last_pts = VLC_TICK_INVALID;
}

/****************************************************************************
 * DecodeBlock:
 ****************************************************************************/
static int DecodeBlock( decoder_t *p_dec, block_t *p_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;

    subpicture_t *p_spu = NULL;

    if( p_block == NULL ) /* No Drain */
        return VLCDEC_SUCCESS;

    if( p_block->i_flags & BLOCK_FLAG_CORRUPTED )
    {
        Flush( p_dec );
        block_Release( p_block );
        return VLCDEC_SUCCESS;
    }

    if( p_block->i_buffer == 0 || p_block->p_buffer[0] == '\0' )
    {
        block_Release( p_block );
        return VLCDEC_SUCCESS;
    }

    if( p_block->i_pts != p_sys->i_last_pts )
    {
        libass_spu_updater_sys_t *p_spu_sys = malloc( sizeof(*p_spu_sys) );
        if( !p_spu_sys )
        {
            block_Release( p_block );
            return VLCDEC_SUCCESS;
        }

        static const struct vlc_spu_updater_ops spu_ops =
        {
            .update   = SubpictureUpdate,
            .destroy  = SubpictureDestroy,
        };

        subpicture_updater_t updater = {
            .sys = p_spu_sys,
            .ops = &spu_ops,
        };

        p_spu = decoder_NewSubpicture( p_dec, &updater );
        if( !p_spu )
        {
            msg_Warn( p_dec, "can't get spu buffer" );
            free( p_spu_sys );
            block_Release( p_block );
            return VLCDEC_SUCCESS;
        }

        p_spu_sys->p_dec_sys = p_sys;
        p_spu_sys->i_pts = p_block->i_pts;
        p_spu->i_start = p_block->i_pts;
        p_spu->i_stop = __MAX( p_sys->i_max_stop, p_block->i_pts + p_block->i_length );
        p_spu->b_ephemer = true;

        p_sys->i_max_stop = p_spu->i_stop;

        DecSysHold( p_sys ); /* Keep a reference for the returned subpicture */
    }

    p_sys->i_last_pts = p_block->i_pts;

    vlc_mutex_lock( &p_sys->lock );
    if( p_sys->p_track )
    {
        ass_process_chunk( p_sys->p_track,(void *) p_block->p_buffer, p_block->i_buffer,
                           MS_FROM_VLC_TICK( p_block->i_pts ), MS_FROM_VLC_TICK( p_block->i_length ) );
    }
    vlc_mutex_unlock( &p_sys->lock );

    block_Release( p_block );

    if( p_spu )
        decoder_QueueSub( p_dec, p_spu );

    return VLCDEC_SUCCESS;
}

/****************************************************************************
 *
 ****************************************************************************/
static void SubpictureUpdate( subpicture_t *p_subpic,
                              const struct vlc_spu_updater_configuration *cfg )
{
    libass_spu_updater_sys_t *p_spusys = p_subpic->updater.sys;
    decoder_sys_t *p_sys = p_spusys->p_dec_sys;
    const video_format_t *p_fmt_src = cfg->video_src;
    const video_format_t *p_fmt_dst = cfg->video_dst;

    bool b_fmt_src = p_fmt_src->i_visible_width  != cfg->prev_src->i_visible_width ||
                     p_fmt_src->i_visible_height != cfg->prev_src->i_visible_height;
    bool b_fmt_dst = !video_format_IsSimilar(cfg->prev_dst, p_fmt_dst);

    vlc_mutex_lock( &p_sys->lock );

    if( b_fmt_src || b_fmt_dst )
    {
        ass_set_frame_size( p_sys->p_renderer, p_fmt_dst->i_visible_width, p_fmt_dst->i_visible_height );
#if LIBASS_VERSION > 0x01010000
        ass_set_storage_size( p_sys->p_renderer, p_fmt_src->i_visible_width, p_fmt_src->i_visible_height );
#endif
        const double src_ratio = (double)p_fmt_src->i_visible_width / p_fmt_src->i_visible_height;
        const double dst_ratio = (double)p_fmt_dst->i_visible_width / p_fmt_dst->i_visible_height;
#if LIBASS_VERSION >= 0x01020000
        ass_set_pixel_aspect( p_sys->p_renderer, dst_ratio / src_ratio );
#else
        ass_set_aspect_ratio( p_sys->p_renderer, dst_ratio / src_ratio, 1 );
#endif
    }

    /* */
    const vlc_tick_t i_stream_date = p_spusys->i_pts + (cfg->pts - p_subpic->i_start);
    int i_changed;
    ASS_Image *p_img = ass_render_frame( p_sys->p_renderer, p_sys->p_track,
                                         MS_FROM_VLC_TICK( i_stream_date ), &i_changed );

    if( !i_changed && !b_fmt_src && !b_fmt_dst &&
        (p_img != NULL) == (!vlc_spu_regions_is_empty(&p_subpic->regions)) )
    {
        vlc_mutex_unlock( &p_sys->lock );
        return;
    }

    vlc_spu_regions_Clear( &p_subpic->regions );

    /* */
    p_subpic->i_original_picture_height = p_fmt_dst->i_visible_height;
    p_subpic->i_original_picture_width = p_fmt_dst->i_visible_width;

    /* XXX to improve efficiency we merge regions that are close minimizing
     * the lost surface.
     * libass tends to create a lot of small regions and thus spu engine
     * reinstanciate a lot the scaler, and as we do not support subpel blending
     * it looks ugly (text unaligned).
     */
    const int i_max_region = 4;
    rectangle_t region[i_max_region];
    const int i_region = BuildRegions( region, i_max_region, p_img, p_fmt_dst->i_width, p_fmt_dst->i_height );

    if( i_region <= 0 )
    {
        vlc_mutex_unlock( &p_sys->lock );
        return;
    }

    /* Allocate the regions and draw them */
    video_format_t fmt_region;
    fmt_region = *p_fmt_dst;
    fmt_region.i_chroma   = VLC_CODEC_RGBA;
    fmt_region.i_x_offset = 0;
    fmt_region.i_y_offset = 0;
    for( int i = 0; i < i_region; i++ )
    {
        subpicture_region_t *r;

        /* */
        fmt_region.i_width =
        fmt_region.i_visible_width  = region[i].x1 - region[i].x0;
        fmt_region.i_height =
        fmt_region.i_visible_height = region[i].y1 - region[i].y0;

        r = subpicture_region_New( &fmt_region );
        if( !r )
            break;
        r->b_absolute = true; r->b_in_window = false;
        r->i_x = region[i].x0;
        r->i_y = region[i].y0;
        r->i_align = SUBPICTURE_ALIGN_TOP | SUBPICTURE_ALIGN_LEFT;

        /* */
        RegionDraw( r, p_img );

        /* */
        vlc_spu_regions_push(&p_subpic->regions, r);
    }
    vlc_mutex_unlock( &p_sys->lock );

}
static void SubpictureDestroy( subpicture_t *p_subpic )
{
    libass_spu_updater_sys_t *p_spusys = p_subpic->updater.sys;

    DecSysRelease( p_spusys->p_dec_sys );
    free( p_spusys );
}

static rectangle_t r_create( int x0, int y0, int x1, int y1 )
{
    rectangle_t r = { x0, y0, x1, y1 };
    return r;
}
static rectangle_t r_img( const ASS_Image *p_img )
{
    return r_create( p_img->dst_x, p_img->dst_y, p_img->dst_x+p_img->w, p_img->dst_y+p_img->h );
}
static void r_add( rectangle_t *r, const rectangle_t *n )
{
    r->x0 = __MIN( r->x0, n->x0 );
    r->y0 = __MIN( r->y0, n->y0 );
    r->x1 = __MAX( r->x1, n->x1 );
    r->y1 = __MAX( r->y1, n->y1 );
}
static int r_surface( const rectangle_t *r )
{
    return (r->x1-r->x0) * (r->y1-r->y0);
}
static bool r_overlap( const rectangle_t *a, const rectangle_t *b, int i_dx, int i_dy )
{
    return  __MAX(a->x0-i_dx, b->x0) < __MIN( a->x1+i_dx, b->x1 ) &&
            __MAX(a->y0-i_dy, b->y0) < __MIN( a->y1+i_dy, b->y1 );
}

static int BuildRegions( rectangle_t *p_region, int i_max_region, ASS_Image *p_img_list, int i_width, int i_height )
{
    ASS_Image *p_tmp;
    int i_count;

#ifdef DEBUG_REGION
    int64_t i_ck_start = vlc_tick_now();
#endif

    for( p_tmp = p_img_list, i_count = 0; p_tmp != NULL; p_tmp = p_tmp->next )
        if( p_tmp->w > 0 && p_tmp->h > 0 )
            i_count++;
    if( i_count <= 0 )
        return 0;

    ASS_Image **pp_img = calloc( i_count, sizeof(*pp_img) );
    if( !pp_img )
        return 0;

    for( p_tmp = p_img_list, i_count = 0; p_tmp != NULL; p_tmp = p_tmp->next )
        if( p_tmp->w > 0 && p_tmp->h > 0 )
            pp_img[i_count++] = p_tmp;

    /* */
    const int i_w_inc = __MAX( ( i_width + 49 ) / 50, 32 );
    const int i_h_inc = __MAX( ( i_height + 99 ) / 100, 32 );
    int i_maxh = i_w_inc;
    int i_maxw = i_h_inc;
    int i_region;
    rectangle_t region[i_max_region+1];

    i_region = 0;
    for( int i_used = 0; i_used < i_count; )
    {
        int n;
        for( n = 0; n < i_count; n++ )
        {
            if( pp_img[n] )
                break;
        }
        assert( i_region < i_max_region + 1 );
        region[i_region++] = r_img( pp_img[n] );
        pp_img[n] = NULL; i_used++;

        bool b_ok;
        do {
            b_ok = false;
            for( n = 0; n < i_count; n++ )
            {
                ASS_Image *p_img = pp_img[n];
                if( !p_img )
                    continue;
                rectangle_t r = r_img( p_img );

                int k;
                int i_best = -1;
                int i_best_s = INT_MAX;
                for( k = 0; k < i_region; k++ )
                {
                    if( !r_overlap( &region[k], &r, i_maxw, i_maxh ) )
                        continue;
                    int s = r_surface( &r );
                    if( s < i_best_s )
                    {
                        i_best_s = s;
                        i_best = k;
                    }
                }
                if( i_best >= 0 )
                {
                    r_add( &region[i_best], &r );
                    pp_img[n] = NULL; i_used++;
                    b_ok = true;
                }
            }
        } while( b_ok );

        if( i_region > i_max_region )
        {
            int i_best_i = -1;
            int i_best_j = -1;
            int i_best_ds = INT_MAX;

            /* merge best */
            for( int i = 0; i < i_region; i++ )
            {
                for( int j = i+1; j < i_region; j++ )
                {
                    rectangle_t rect = region[i];
                    r_add( &rect, &region[j] );
                    int ds = r_surface( &rect ) - r_surface( &region[i] ) - r_surface( &region[j] );

                    if( ds < i_best_ds )
                    {
                        i_best_i = i;
                        i_best_j = j;
                        i_best_ds = ds;
                    }
                }
            }
#ifdef DEBUG_REGION
            msg_Err( p_spu, "Merging %d and %d", i_best_i, i_best_j );
#endif
            if( i_best_j >= 0 && i_best_i >= 0 )
            {
                r_add( &region[i_best_i], &region[i_best_j] );

                if( i_best_j+1 < i_region )
                    memmove( &region[i_best_j], &region[i_best_j+1], sizeof(*region) * ( i_region - (i_best_j+1)  ) );
                i_region--;
            }
        }
    }

    /* */
    for( int n = 0; n < i_region; n++ )
        p_region[n] = region[n];

#ifdef DEBUG_REGION
    int64_t i_ck_time = vlc_tick_now() - i_ck_start;
    msg_Err( p_spu, "ASS: %d objects merged into %d region in %d micros", i_count, i_region, (int)(i_ck_time) );
#endif

    free( pp_img );

    return i_region;
}

static void RegionDraw( subpicture_region_t *p_region, ASS_Image *p_img )
{
    const plane_t *p = &p_region->p_picture->p[0];
    const int i_x = p_region->i_x;
    const int i_y = p_region->i_y;
    const int i_width  = p_region->p_picture->format.i_width;
    const int i_height = p_region->p_picture->format.i_height;

    memset( p->p_pixels, 0x00, p->i_pitch * p->i_visible_lines );
    for( ; p_img != NULL; p_img = p_img->next )
    {
        int i_dst_x = p_img->dst_x - i_x;
        int i_dst_y = p_img->dst_y - i_y;
        if( i_dst_x < 0 || i_dst_x + p_img->w > i_width ||
            i_dst_y < 0 || i_dst_y + p_img->h > i_height )
            continue;

        /* /!\ Bogus alpha channel is inverted */
        const unsigned a = (~p_img->color      )&0xff;
        if( a == 0 )
            continue;

        const unsigned r = (p_img->color >> 24)&0xff;
        const unsigned g = (p_img->color >> 16)&0xff;
        const unsigned b = (p_img->color >>  8)&0xff;

        const uint8_t *srcrow = p_img->bitmap;
        int i_pitch_src = p_img->stride;
        int i_pitch_dst = p->i_pitch;
        uint8_t *dstrow = &p->p_pixels[i_dst_y * i_pitch_dst + 4 * i_dst_x];

        for( int y = 0; y < p_img->h; y++ )
        {
            const uint8_t *src = srcrow;
            uint8_t *dst = dstrow;

            for( int x = 0; x < p_img->w; x++ )
            {
                unsigned opacity = *src++; /* 1 Bpp channel */
                if( opacity != 0 ) /* we need to blend only non transparent content */
                {
                    unsigned i_an = a * opacity / 255U;
                    unsigned i_ao = dst[3];
                    if( i_ao == 0 )
                    {
                        dst[0] = r;
                        dst[1] = g;
                        dst[2] = b;
                        dst[3] = i_an;
                    }
                    else
                    {
                        unsigned i_ani = 255 - i_an;
                        dst[3] = 255 - (255 - i_ao) * i_ani / 255;
                        if( dst[3] != 0 )
                        {
                            unsigned i_aoni = i_ao * i_ani / 255;
                            dst[0] = ( dst[0] * i_aoni + r * i_an ) / dst[3];
                            dst[1] = ( dst[1] * i_aoni + g * i_an ) / dst[3];
                            dst[2] = ( dst[2] * i_aoni + b * i_an ) / dst[3];
                        }
                    }
                }
                dst += 4;
            }
            srcrow += i_pitch_src;
            dstrow += i_pitch_dst;
        }
    }

#ifdef DEBUG_REGION
    /* XXX Draw a box for debug */
#define P(x,y) ((uint32_t*)&p->p_pixels[(y)*p->i_pitch + 4*(x)])
    for( int y = 0; y < p->i_lines; y++ )
        *P(0,y) = *P(p->i_visible_pitch/4-1,y) = 0xff000000;
    for( int x = 0; x < p->i_visible_pitch; x++ )
        *P(x/4,0) = *P(x/4,p->i_visible_lines-1) = 0xff000000;
#undef P
#endif
}

/* Patch [Script Info] aiming old and custom rendering engine
 * See #27771 */
static void OldEngineClunkyRollInfoPatch( decoder_t *p_dec, ASS_Track *p_track )
{
    if( !p_dec->fmt_in->i_extra )
        return;

    stream_t *p_memstream = vlc_stream_MemoryNew( p_dec, p_dec->fmt_in->p_extra,
                                                  p_dec->fmt_in->i_extra, true );
    char *s = vlc_stream_ReadLine( p_memstream );
    unsigned playres[2] = {0, 0};
    bool b_hotfix = false;
    if( s && !strncmp( s, "[Script Info]", 13 ) )
    {
        free( s );
        for( ;; )
        {
            s = vlc_stream_ReadLine( p_memstream );
            if( !s || *s == '[' /* Next section */ )
            {
                break;
            }
            else if( !strncmp( s, "PlayResX: ", 10 ) ||
                     !strncmp( s, "PlayResY: ", 10 ) )
            {
                playres[s[7] - 'X'] = atoi( &s[9] );
            }
            else if( !strncmp( s, "Original Script: ", 17 ) )
            {
                b_hotfix = !!strstr( s, "[http://www.crunchyroll.com/user/" );
                if( !b_hotfix )
                    break;
            }
            else if( !strncmp( s, "LayoutRes", 9 ) ||
                     !strncmp( s, "ScaledBorderAndShadow:", 22  ) )
            {
                /* They can still have fixed their mess in the future. Tell me, Marty. */
                b_hotfix = false;
                break;
            }
            free( s );
        }
    }
    free( s );
    vlc_stream_Delete( p_memstream );
    if( b_hotfix && playres[0] && playres[1] )
    {
	msg_Dbg( p_dec,"patching script info for custom rendering engine "
                       "(built against libass 0x%X)", LIBASS_VERSION );
        /* Only modify struct _before_ any ass_process_chunk calls
           (see ass_types.h documentation for when modifications are allowed) */
        p_track->ScaledBorderAndShadow = 1;
        p_track->YCbCrMatrix = YCBCR_NONE;
#if LIBASS_VERSION >= 0x01600020
        p_track->LayoutResX = playres[0];
        p_track->LayoutResY = playres[1];
#endif
    }
}
