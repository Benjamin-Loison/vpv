#include <cstdlib>
#include <cstring>
#include <glob.h>
#include <algorithm>

#include <SFML/OpenGL.hpp>

#include "Sequence.hpp"
#include "Player.hpp"
#include "View.hpp"
#include "Colormap.hpp"
#include "Image.hpp"
#include "alphanum.hpp"
#include "globals.hpp"

const char* getGLError(GLenum error)
{
#define casereturn(x) case x: return #x
    switch (error) {
        casereturn(GL_INVALID_ENUM);
        casereturn(GL_INVALID_VALUE);
        casereturn(GL_INVALID_OPERATION);
        casereturn(GL_INVALID_FRAMEBUFFER_OPERATION);
        casereturn(GL_OUT_OF_MEMORY);
        default:
        case GL_NO_ERROR:
        return "";
    }
#undef casereturn
}

#define GLDEBUG(x) \
    x; \
{ \
    GLenum e; \
    while((e = glGetError()) != GL_NO_ERROR) \
    { \
        printf("%s:%s:%d for call %s", getGLError(e), __FILE__, __LINE__, #x); \
    } \
}

Sequence::Sequence()
{
    static int id = 0;
    id++;
    ID = "Sequence " + std::to_string(id);

    view = nullptr;
    player = nullptr;
    colormap = nullptr;
    image = nullptr;

    valid = false;
    force_reupload = false;

    loadedFrame = -1;
    loadedRect = ImRect();

    glob.reserve(4096);
    glob_.reserve(4096);
    glob = "";
    glob_ = "";
}

void Sequence::loadFilenames() {
    glob_t res;
    ::glob(glob.c_str(), GLOB_TILDE | GLOB_NOSORT, NULL, &res);
    filenames.resize(res.gl_pathc);
    for(unsigned int j = 0; j < res.gl_pathc; j++) {
        filenames[j] = res.gl_pathv[j];
    }
    globfree(&res);
    std::sort(filenames.begin(), filenames.end(), doj::alphanum_less<std::string>());

    if (filenames.empty() && !strcmp(glob.c_str(), "-")) {
        filenames.push_back("-");
    }

    valid = filenames.size() > 0;
    strcpy(&glob_[0], &glob[0]);

    loadedFrame = -1;
    if (player)
        player->reconfigureBounds();
}

void Sequence::loadTextureIfNeeded()
{
    if (valid && player) {
        int frame = player->frame;
        assert(frame > 0);

        if (loadedFrame != player->frame || force_reupload) {
            loadedRect = ImRect();
            if (!useCache && image) {
                delete image;
            }
            image = nullptr;
        }

        const Image* img = getCurrentImage();
        if (!img)
            return;

        if (loadedFrame != player->frame) {
            printf("%s (%dx%dx%d) [%g..%g]\n", filenames[frame - 1].c_str(), img->w, img->h, img->format, img->min, img->max);
        }

        int w = img->w;
        int h = img->h;
        bool reupload = false || force_reupload;

        ImVec2 u, v;
        view->compute(ImVec2(w, h), u, v);

        ImRect area(u.x*w, u.y*h, v.x*w+1, v.y*h+1);
        area.Floor();
        area.Clip(ImRect(0, 0, w, h));

        area.Expand(32);  // to avoid multiple uploads during zoom-out
        area.Clip(ImRect(0, 0, w, h));

        if (!loadedRect.ContainsInclusive(area)) {
            reupload = true;
        }

        if (reupload) {
            unsigned int gltype;
            if (img->type == Image::UINT8)
                gltype = GL_UNSIGNED_BYTE;
            else if (img->type == Image::FLOAT)
                gltype = GL_FLOAT;
            else
                assert(0);

            unsigned int glformat;
            if (img->format == Image::R)
                glformat = GL_RED;
            else if (img->format == Image::RG)
                glformat = GL_RG;
            else if (img->format == Image::RGB)
                glformat = GL_RGB;
            else if (img->format == Image::RGBA)
                glformat = GL_RGBA;
            else
                assert(0);

            if (texture.id == -1 || texture.size.x != w || texture.size.y != h
                || texture.type != gltype || texture.format != glformat) {
                texture.create(w, h, gltype, glformat);
            }

            size_t elsize;
            if (img->type == Image::UINT8) elsize = sizeof(uint8_t);
            else if (img->type == Image::FLOAT) elsize = sizeof(float);
            else assert(0);
            const uint8_t* data = (uint8_t*) img->pixels + elsize*(w * (int)area.Min.y + (int)area.Min.x)*img->format;

            if (area.GetWidth() > 0 && area.GetHeight() > 0) {
                glBindTexture(GL_TEXTURE_2D, texture.id);
                GLDEBUG();

                glPixelStorei(GL_UNPACK_ROW_LENGTH, w);
                GLDEBUG();
                glTexSubImage2D(GL_TEXTURE_2D, 0, area.Min.x, area.Min.y, area.GetWidth(), area.GetHeight(),
                                glformat, gltype, data);
                GLDEBUG();
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                GLDEBUG();
            }

            loadedFrame = frame;
            loadedRect.Add(area);
            force_reupload = false;
        }
    }
}

void Sequence::autoScaleAndBias()
{
    colormap->scale = 1.f;
    colormap->bias = 0.f;

    const Image* img = getCurrentImage();
    if (!img)
        return;

    colormap->scale = 1.f / (img->max - img->min);
    colormap->bias = - img->min * colormap->scale;
}

void Sequence::smartAutoScaleAndBias(ImVec2& p1, ImVec2& p2)
{
    colormap->scale = 1.f;
    colormap->bias = 0.f;

    const Image* img = getCurrentImage();
    if (!img)
        return;

    if (p1.x < 0) p1.x = 0;
    if (p1.y < 0) p1.y = 0;
    if (p2.x >= img->w) p2.x = img->w - 1;
    if (p2.y >= img->h) p2.y = img->h - 1;

    float min;
    float max;
    const float* data = (const float*) img->pixels;
    for (int y = p1.y; y < p2.y; y++) {
        for (int x = p1.x; x < p2.x; x++) {
            for (int d = 0; d < img->format; d++) {
                min = std::min(min, data[d + img->format*(x+y*img->w)]);
                max = std::max(max, data[d + img->format*(x+y*img->w)]);
            }
        }
    }

    colormap->scale = 1.f / (max - min);
    colormap->bias = - min * colormap->scale;
}

const Image* Sequence::getCurrentImage() {
    if (!valid || !player) {
        return 0;
    }

    if (!image) {
        int frame = player->frame;
        const Image* img = Image::load(filenames[frame - 1]);
        image = img;
    }

    return image;
}

const std::string Sequence::getTitle() const
{
    std::string seqname = std::string(glob.c_str());
    if (!valid)
        return "(the sequence '" + seqname + "' contains no images)";
    if (!player)
        return "(no player associated with the sequence '" + seqname + "')";
    if (!colormap)
        return "(no colormap associated with the sequence '" + seqname + "')";

    std::string title;
    title += "[" + std::to_string(player->frame) + '/' + std::to_string(filenames.size()) + "]";
    title += " " + filenames[player->frame - 1];
    if (image) {
        title += " (" + std::to_string(image->w) + "x" + std::to_string(image->h) + "x" + std::to_string(image->format) + ")";
        title += " [" + std::to_string(image->min) + ".." + std::to_string(image->max) + "]";
        title += " shader:" + colormap->getShaderName();
    } else {
        title += " cannot be loaded";
    }
    return title;
}

