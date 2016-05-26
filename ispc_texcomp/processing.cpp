﻿#include "stdafx.h"
#include "DirectXTex.h"
#include "ispc_texcomp.h"
#include "processing.h"

#define ISALIGN(x, a)           (((uintptr_t)(x) & ((a) - 1)) == 0)
#define SAFE_DELETE_ARRAY(x)    if((x)) { delete [] (x); (x) = NULL; }

namespace {

const wchar_t how_to_use[] = {
    _T("ispc_texcomp\n")
    _T("\n")
    _T("  Usage:\n")
    _T("    ispc_texcomp [Options] -i <filename> -o <filename>\n")
    _T("\n")
    _T("  Options:\n")
    _T("    -h, --help      このメッセージです\n")
    _T("    -v, --verbose   詳細表示モード\n")
    _T("    -f <format>     圧縮フォーマット\n")
    _T("    -l <level>      圧縮レベル(bc6h,bc7)\n")
    _T("    -rgb            アルファチャンネルを無視する(bc7のみ)\n")
    _T("    -dxtex          ISPC Texture Compressorを使用しない\n")
    _T("\n")
    _T("  <format>\n")
    _T("    bc1,bc3,bc6h,bc7\n")
    _T("\n")
    _T("  <level>\n")
    _T("    ultrafast,veryfast,fast,basic,slow,veryslow\n")
    _T("\n")
};

enum class Format
{
    BC1,
    BC3,
    BC6H,
    BC7,
};

const wchar_t* formats[] = {
    _T("bc1"),
    _T("bc3"),
    _T("bc6h"),
    _T("bc7"),
};

enum class Level
{
    UltraFast,
    VeryFast,
    Fast,
    Basic,
    Slow,
    VerySlow,
};

const wchar_t* levels[] = {
    _T("ultrafast"),
    _T("veryfast"),
    _T("fast"),
    _T("basic"),
    _T("slow"),
    _T("veryslow"),
};

bool is_block_align( Format fmt, size_t width, size_t height )
{
    size_t block_align = fmt == Format::BC1 ? 4 : 8;
    return ISALIGN(width, block_align) /*&& ISALIGN(height, block_align)*/;
}

size_t get_memory_size( uint32_t width, uint32_t height, Format format )
{
    switch ( format ) {
    case Format::BC1:
        return (((width + 3) / 4) * ((height + 3) / 4)) * 8 /** depth*/;
    case Format::BC3:
    case Format::BC6H:
    case Format::BC7:
        return (((width + 3) / 4) * ((height + 3) / 4)) * 16 /** depth*/;
    default:
        break;
    }
    return width * height * 4;
}

void init_bc6h_enc_settings( bc6h_enc_settings* settings, Level level )
{
    switch ( level ) {
    case Level::UltraFast:  GetProfile_bc6h_veryfast( settings ); break;
    case Level::VeryFast:   GetProfile_bc6h_veryfast( settings ); break;
    case Level::Fast:       GetProfile_bc6h_fast( settings ); break;
    case Level::Basic:      GetProfile_bc6h_basic( settings ); break;
    case Level::Slow:       GetProfile_bc6h_slow( settings ); break;
    case Level::VerySlow:   GetProfile_bc6h_veryslow( settings ); break;
    default:
        break;
    }
}

void init_bc7_enc_settings( bc7_enc_settings* settings, Level level, bool alpha_ignored )
{
    if ( alpha_ignored ) {
        switch ( level ) {
        case Level::UltraFast:  GetProfile_ultrafast( settings ); break;
        case Level::VeryFast:   GetProfile_veryfast( settings ); break;
        case Level::Fast:       GetProfile_fast( settings ); break;
        case Level::Basic:      GetProfile_basic( settings ); break;
        case Level::Slow:       GetProfile_slow( settings ); break;
        case Level::VerySlow:   GetProfile_slow( settings ); break;
        default:
            break;
        }
    }
    else {
        switch ( level ) {
        case Level::UltraFast:  GetProfile_alpha_ultrafast( settings ); break;
        case Level::VeryFast:   GetProfile_alpha_veryfast( settings ); break;
        case Level::Fast:       GetProfile_alpha_fast( settings ); break;
        case Level::Basic:      GetProfile_alpha_basic( settings ); break;
        case Level::Slow:       GetProfile_alpha_slow( settings ); break;
        case Level::VerySlow:   GetProfile_alpha_slow( settings ); break;
        default:
            break;
        }
    }
}

DXGI_FORMAT format_to_dxgiformat( Format format )
{
    switch ( format ) {
    case Format::BC1:   return DXGI_FORMAT_BC1_UNORM;
    case Format::BC3:   return DXGI_FORMAT_BC3_UNORM;
    case Format::BC6H:  return DXGI_FORMAT_BC6H_UF16;
    case Format::BC7:
    default:
        break;
    }
    return DXGI_FORMAT_BC7_UNORM;
}

}

namespace ispc_texcomp {

void show_usage()
{
    printf( how_to_use );
}

struct Spec
{
    bool verbose;
    bool rgb_mode;
    bool dxtex_mode;
    Format format;
    Level level;
    String input_name;
    String output_name;

    Spec()
        : verbose(false), rgb_mode(false), dxtex_mode(false), format(Format::BC3), level(Level::Basic) {}
};

std::shared_ptr<Spec> init_spec_from( const CommandLineOptions& options )
{
    std::shared_ptr<Spec> spec;
    if ( !options.empty() ) {
        spec = std::make_shared<Spec>();
        for ( const auto& kv : options ) {
            auto& key = kv.first;
            if ( key == _T("-h") || key == _T("--help") ) {
                return nullptr;
            }
            else if ( key == _T("-v") || key == _T("--verbose") ) {
                spec->verbose = true;
            }
            else if ( key == _T("-f") ) {
                auto& args = kv.second;
                if ( args.size() >= 1 ) {
                    auto& fmt = args[0];
                    for ( int32_t i = 0; i < ARRAYSIZE(formats); ++i ) {
                        if ( fmt == formats[i] ) {
                            spec->format = static_cast<Format>(i);
                            break;
                        }
                    }
                }
            }
            else if ( key == _T("-l") ) {
                auto& args = kv.second;
                if ( args.size() >= 1 ) {
                    auto& lv = args[0];
                    for ( int32_t i = 0; i < ARRAYSIZE(levels); ++i ) {
                        if ( lv == levels[i] ) {
                            spec->level = static_cast<Level>(i);
                            break;
                        }
                    }
                }
            }
            else if ( key == _T("-rgb") ) {
                spec->rgb_mode = true;
            }
            else if ( key == _T("-dxtex") ) {
                spec->dxtex_mode = true;
            }
            else if ( key == _T("-i") ) {
                if ( kv.second.size() >= 1 ) {
                    spec->input_name = kv.second[0];
                }
            }
            else if ( key == _T("-o") ) {
                if ( kv.second.size() >= 1 ) {
                    spec->output_name = kv.second[0];
                }
            }
        }
    }
    return spec;
}

bool compress_and_save( const std::shared_ptr<Spec> spec )
{
    HRESULT hr;

    hr = ::CoInitializeEx( NULL, COINIT_MULTITHREADED );
    if ( FAILED(hr) ) {
        printf( _T("  CoInitializeEx failed. ret = 0x%x\n"), hr );
        return false;
    }

    DirectX::TexMetadata meta;
    std::unique_ptr<DirectX::ScratchImage> image( new DirectX::ScratchImage );
    hr = DirectX::LoadFromTGAFile( spec->input_name.c_str(), &meta, *image );
    if ( FAILED(hr) ) {
        hr = DirectX::LoadFromWICFile( spec->input_name.c_str(), DirectX::WIC_FLAGS_NONE, &meta, *image );
        if ( FAILED(hr) ) {
            printf( _T("  DirectX::LoadFromXXXFile failed. [%s]\n"), spec->input_name.c_str() );
            return false;
        }
    }

    if ( !is_block_align( spec->format, meta.width, meta.height ) ) {
        printf( _T("  Input width and height need to be a multiple of block size.\n") );
        printf( _T("  4 bytes/block for BC1/ETC1, 8 bytes/block for BC3/BC6H/BC7/ASTC,\n") );
        printf( _T("  the blocks are stored in raster scan order (natural CPU texture layout).") );
        return false;
    }

    if ( meta.format != DXGI_FORMAT_R8G8B8A8_UNORM ) {
        std::unique_ptr<DirectX::ScratchImage> temp( new DirectX::ScratchImage );
        hr = DirectX::Convert( image->GetImages(), image->GetImageCount(),
            image->GetMetadata(),
            DXGI_FORMAT_R8G8B8A8_UNORM, DirectX::TEX_FILTER_DEFAULT, 0.5f, *temp );
        if ( FAILED(hr) ) {
            printf( _T("  DirectX::Convert failed. ret = 0x%x\n"), hr );
            return false;
        }
        image = std::move( temp );
    }

    if ( !spec->dxtex_mode ) {
        uint32_t width = static_cast<uint32_t>(image->GetMetadata().width);
        uint32_t height = static_cast<uint32_t>(image->GetMetadata().height);

        DirectX::Blob blob;
        hr = blob.Initialize( get_memory_size( width, height, spec->format ) );
        if ( FAILED(hr) ) {
            printf( _T("  DirectX::Blob::Initialize failed. ret = 0x%x\n"), hr );
            return false;
        }

        rgba_surface surface;
        surface.ptr = image->GetPixels();
        surface.width = width;
        surface.height = height;
        surface.stride = width * 4; // format is must be DXGI_FORMAT_R8G8B8A8_UNORM

        switch ( spec->format ) {
        case Format::BC1:
            CompressBlocksBC1( &surface, static_cast<uint8_t*>(blob.GetBufferPointer()) );
            break;

        case Format::BC3:
            CompressBlocksBC3( &surface, static_cast<uint8_t*>(blob.GetBufferPointer()) );
            break;

        case Format::BC6H:
            {
                bc6h_enc_settings setting;
                init_bc6h_enc_settings( &setting, spec->level );
                CompressBlocksBC6H( &surface, static_cast<uint8_t*>(blob.GetBufferPointer()), &setting );
            }
            break;

        case Format::BC7:
            {
                bc7_enc_settings setting;
                init_bc7_enc_settings( &setting, spec->level, spec->rgb_mode );
                CompressBlocksBC7( &surface, static_cast<uint8_t*>(blob.GetBufferPointer()), &setting );
            }
            break;
        }

        hr = image->Initialize2D( format_to_dxgiformat( spec->format ), width, height, 1, 1 );
        if ( FAILED(hr) ) {
            printf( _T("  DirectX::ScratchImage::Initialize2D failed. ret = 0x%x\n"), hr );
            return false;
        }

        memcpy( image->GetPixels(), blob.GetBufferPointer(), blob.GetBufferSize() );
    }
    else {
        std::unique_ptr<DirectX::ScratchImage> temp( new DirectX::ScratchImage );
        hr = DirectX::Compress( image->GetImages(), image->GetImageCount(), image->GetMetadata(), format_to_dxgiformat( spec->format ), DirectX::TEX_COMPRESS_PARALLEL, 0.5f, *temp );
        image = std::move( temp );
    }

    hr = DirectX::SaveToDDSFile( image->GetImages(), image->GetImageCount(), image->GetMetadata(), DirectX::DDS_FLAGS_NONE, spec->output_name.c_str() );
    if ( FAILED(hr) ) {
        printf( _T("  DirectX::SaveToDDSFile failed. ret = 0x%x\n"), hr );
        return false;
    }

    ::CoUninitialize();

    printf( _T("done.\n") );

    return true;
}

}
