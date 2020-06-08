
#include "Python.h"
#include <stdlib.h>
#include <stdio.h>
#include <../openjpeg/src/lib/openjp2/openjpeg.h>


// Size of the buffer for the input stream
#define BUFFER_SIZE OPJ_J2K_STREAM_CHUNK_SIZE


const char * OpenJpegVersion(void)
{
    /* Return the openjpeg version as char array

    Returns
    -------
    char *
        The openjpeg version as MAJOR.MINOR.PATCH
    */
    return opj_version();
}


// parameters for decoding
typedef struct opj_decompress_params {
    /** core library parameters */
    opj_dparameters_t core;

    /** input file format 0: J2K, 1: JP2, 2: JPT */
    int decod_format;

    /** Decoding area left boundary */
    OPJ_UINT32 DA_x0;
    /** Decoding area right boundary */
    OPJ_UINT32 DA_x1;
    /** Decoding area up boundary */
    OPJ_UINT32 DA_y0;
    /** Decoding area bottom boundary */
    OPJ_UINT32 DA_y1;
    /** Verbose mode */
    OPJ_BOOL m_verbose;

    /** tile number of the decoded tile */
    OPJ_UINT32 tile_index;
    /** Nb of tile to decode */
    OPJ_UINT32 nb_tile_to_decode;

    /* force output colorspace to RGB */
    //int force_rgb;
    /** number of threads */
    int num_threads;
    /* Quiet */
    //int quiet;
    /** number of components to decode */
    OPJ_UINT32 numcomps;
    /** indices of components to decode */
    OPJ_UINT32* comps_indices;
} opj_decompress_parameters;


// Python stream methods
static Py_ssize_t py_tell(PyObject *stream)
{
    /* Return the current position of the `stream`.

    Parameters
    ----------
    stream : PyObject *
        The Python stream object to use (must have a ``tell()`` method).

    Returns
    -------
    Py_ssize_t
        The new position in the `stream`.
    */
    PyObject *result;
    Py_ssize_t location;

    result = PyObject_CallMethod(stream, "tell", NULL);
    location = PyLong_AsSsize_t(result);

    Py_DECREF(result);
    return location;
}


static OPJ_SIZE_T py_read(void *destination, Py_ssize_t nr_bytes, PyObject *fd)
{
    /* Read `nr_bytes` from Python object `fd` and copy it to `destination`.

    Parameters
    ----------
    destination : void *
        The object where the read data will be copied.
    nr_bytes : Py_ssize_t
        The number of bytes to be read.
    fd : PyObject *
        The Python file-like to read the data from (must have a ``read()``
        method).

    Returns
    -------
    int
        The number of bytes read or -1 if reading failed.
    */
    PyObject* result;
    char* buffer;
    OPJ_SIZE_T length;
    int bytes_result;
    Py_ssize_t pos;

    result = PyObject_CallMethod(fd, "read", "n", nr_bytes);
    bytes_result = PyBytes_AsStringAndSize(result, &buffer, &length);

    if (bytes_result == -1) {
        goto error;
    }

    if (length > nr_bytes) {
        goto error;
    }

    memcpy(destination, buffer, length);

    Py_DECREF(result);
    return length;

error:
    //printf("read_data::Error reading data");
    Py_DECREF(result);
    return -1;
}


static OPJ_BOOL py_seek(OPJ_OFF_T offset, PyObject *stream, int whence)
{
    /* Change the `stream` position to the given `offset` from `whence`.

    Parameters
    ----------
    offset : OPJ_OFF_T
        The offset relative to `whence`.
    stream : PyObject *
        The Python stream object to seek (must have a ``seek()`` method).
    whence : int
        0 for SEEK_SET, 1 for SEEK_CUR, 2 for SEEK_END

    Returns
    -------
    OPJ_TRUE : OBJ_BOOL
    */
    // Python and C; SEEK_SET is 0:
    // n: convert C Py_ssize_t to a Python integer
    // i: convert C int to a Python integer
    PyObject *result;
    result = PyObject_CallMethod(stream, "seek", "ni", offset, whence);
    Py_DECREF(result);

    return OPJ_TRUE;
}


static OPJ_BOOL py_seek_set(OPJ_OFF_T offset, PyObject *stream)
{
    /* Change the `stream` position to the given `offset` from SEEK_SET.

    Parameters
    ----------
    offset : OPJ_OFF_T
        The offset relative to SEEK_SET.
    stream : PyObject *
        The Python stream object to seek (must have a ``seek()`` method).

    Returns
    -------
    OPJ_TRUE : OBJ_BOOL
    */
    // Python and C; SEEK_SET is 0:
    // n: convert C Py_ssize_t to a Python integer
    // i: convert C int to a Python integer
    //PyObject *result;
    //result = PyObject_CallMethod(stream, "seek", "ni", offset, SEEK_SET);
    //Py_DECREF(result);

    return py_seek(offset, stream, SEEK_SET);
}


static OPJ_OFF_T py_skip(OPJ_OFF_T offset, PyObject *stream)
{
    /* Change the `stream` position by `offset` from SEEK_CUR and return the
    new position.

    Parameters
    ----------
    offset : OPJ_OFF_T
        The offset relative to SEEK_CUR.
    stream : PyObject *
        The Python stream object to seek (must have a ``seek()`` method).

    Returns
    -------
    off_t
        The new position in the `stream`.
    */
    py_seek(offset, stream, SEEK_CUR);

    off_t pos;
    pos = py_tell(stream);

    return pos ? pos : (OPJ_OFF_T) -1;
}


static OPJ_UINT64 py_length(PyObject * stream)
{
    /* Return the total length of the `stream`.

    Parameters
    ----------
    stream : PyObject *
        The Python stream object (must have ``seek()`` and ``tell()`` methods).

    Returns
    -------
    OPJ_UINT64
        The total length of the `stream`.
    */
    OPJ_OFF_T input_length = 0;

    py_seek(0, stream, SEEK_END);
    input_length = (OPJ_OFF_T)py_tell(stream);
    py_seek(0, stream, SEEK_SET);

    return (OPJ_UINT64)input_length;
}


// OJ
static void set_default_parameters(opj_decompress_parameters* parameters)
{
    if (parameters) {
        memset(parameters, 0, sizeof(opj_decompress_parameters));

        /* default decoding parameters (command line specific) */
        parameters->decod_format = -1;
        /* default decoding parameters (core) */
        opj_set_default_decoder_parameters(&(parameters->core));
    }
}


static void destroy_parameters(opj_decompress_parameters* parameters)
{
    if (parameters) {
        free(parameters->comps_indices);
        parameters->comps_indices = NULL;
    }
}


extern int get_parameters(PyObject* fd, int codec_format)
{
    /* Decode a JPEG 2000 header for the image meta data.

    Parameters
    ----------
    fd : PyObject *
        The Python stream object containing the JPEG 2000 data to be decoded.
    codec_format : int
        The format of the JPEG 2000 data, one of:
        * ``0`` - OPJ_CODEC_J2K : JPEG-2000 codestream
        * ``1`` - OPJ_CODEC_JPT : JPT-stream (JPEG 2000, JPIP)
        * ``2`` - OPJ_CODEC_JP2 : JP2 file format
    */

    // J2K stream
    opj_stream_t *stream = NULL;
    // struct defining image data and characteristics
    opj_image_t *image = NULL;
    // J2K codec
    opj_codec_t *codec = NULL;
    // Setup decompression parameters
    opj_decompress_parameters parameters;
    set_default_parameters(&parameters);

    // Creates an abstract input stream; allocates memory
    stream = opj_stream_create(BUFFER_SIZE, OPJ_TRUE);

    if (!stream) {
        goto quick_exit;
    }

    // Functions for the stream
    opj_stream_set_read_function(stream, py_read);
    opj_stream_set_skip_function(stream, py_skip);
    opj_stream_set_seek_function(stream, py_seek_set);
    opj_stream_set_user_data(stream, fd, NULL);
    opj_stream_set_user_data_length(stream, py_length(fd));

    //opj_set_error_handler(codec, j2k_error, 00);

    codec = opj_create_decompress(codec_format);

    /* Setup the decoder parameters */
    if (!opj_setup_decoder(codec, &(parameters.core)))
    {
        fprintf(stderr, "ERROR -> opj_decompress: failed to setup the decoder\n");
        goto quick_exit;
    }

    if (parameters.num_threads >= 1 && !opj_codec_set_threads(codec, parameters.num_threads))
    {
        fprintf(stderr, "ERROR -> opj_decompress: failed to set number of threads\n");
        goto quick_exit;
    }

    /* Read the main header of the codestream and if necessary the JP2 boxes*/
    // Working! Excellent progress... :p
    if (! opj_read_header(stream, codec, &image)) {
        fprintf(stderr, "ERROR -> opj_decompress: failed to read the header\n");
        goto quick_exit;
    }

    printf("Number of components: %d\n", image->numcomps);
    printf("Number of threads: %d\n", parameters.num_threads);
    printf("Colour space: %d\n", image->color_space);
    printf("Size: %d x %d\n", image->x1, image->y1);
    printf("Number of tiles to decode: %d\n", parameters.nb_tile_to_decode);

    for (unsigned int c_index = 0; c_index < image->numcomps; c_index++)
    {
        int width = (int)image->comps[c_index].w;
        int height = (int)image->comps[c_index].h;
        int precision = (int)image->comps[c_index].prec;
        int nr_bytes = ((precision + 7) & -8) / 8;

        printf(
            "Component %u characteristics: %d x %d, %s %d bit, %d byte\n",
            c_index, width, height,
            image->comps[c_index].sgnd == 1 ? "signed" : "unsigned",
            precision,
            nr_bytes
        );
    }

    destroy_parameters(&parameters);
    opj_destroy_codec(codec);
    opj_image_destroy(image);
    opj_stream_destroy(stream);

    return 1;

    quick_exit:
        destroy_parameters(&parameters);
        if (codec)
            opj_destroy_codec(codec);
        if (image)
            opj_image_destroy(image);
        if (stream)
            opj_stream_destroy(stream);

        return -1;
}


extern int decode(PyObject* fd, unsigned char *out, int codec_format)
{
    /* Decode JPEG 2000 data.

    Parameters
    ----------
    fd : PyObject *
        The Python stream object containing the JPEG 2000 data to be decoded.
    out : unsigned char *
        The numpy ndarray of uint8 where the decoded image data will be written
    codec_format : int
        The format of the JPEG 2000 data, one of:
        * ``0`` - OPJ_CODEC_J2K : JPEG-2000 codestream
        * ``1`` - OPJ_CODEC_JPT : JPT-stream (JPEG 2000, JPIP)
        * ``2`` - OPJ_CODEC_JP2 : JP2 file format
    */

    // J2K stream
    opj_stream_t *stream = NULL;
    // struct defining image data and characteristics
    opj_image_t *image = NULL;
    // J2K codec
    opj_codec_t *codec = NULL;
    // Setup decompression parameters
    opj_decompress_parameters parameters;
    set_default_parameters(&parameters);

    // Creates an abstract input stream; allocates memory
    stream = opj_stream_create(BUFFER_SIZE, OPJ_TRUE);

    if (!stream) {
        goto quick_exit;
    }

    // Functions for the stream
    opj_stream_set_read_function(stream, py_read);
    opj_stream_set_skip_function(stream, py_skip);
    opj_stream_set_seek_function(stream, py_seek_set);
    opj_stream_set_user_data(stream, fd, NULL);
    opj_stream_set_user_data_length(stream, py_length(fd));

    //opj_set_error_handler(codec, j2k_error, 00);

    codec = opj_create_decompress(codec_format);

    /* Setup the decoder parameters */
    if (!opj_setup_decoder(codec, &(parameters.core)))
    {
        fprintf(stderr, "ERROR -> opj_decompress: failed to setup the decoder\n");
        goto quick_exit;
    }

    if (parameters.num_threads >= 1 && !opj_codec_set_threads(codec, parameters.num_threads))
    {
        fprintf(stderr, "ERROR -> opj_decompress: failed to set number of threads\n");
        goto quick_exit;
    }

    /* Read the main header of the codestream and if necessary the JP2 boxes*/
    // Working! Excellent progress... :p
    if (! opj_read_header(stream, codec, &image)) {
        fprintf(stderr, "ERROR -> opj_decompress: failed to read the header\n");
        goto quick_exit;
    }

    printf("Number of components: %d\n", image->numcomps);
    printf("Number of threads: %d\n", parameters.num_threads);
    printf("Colour space: %d\n", image->color_space);
    printf("Size: %d x %d\n", image->x1, image->y1);
    printf("Number of tiles to decode: %d\n", parameters.nb_tile_to_decode);

    if (parameters.numcomps) {
        if (!opj_set_decoded_components(
                codec, parameters.numcomps, parameters.comps_indices, OPJ_FALSE
            ))
        {
            fprintf(stderr, "ERROR -> opj_decompress: failed to set the component indices!\n");
            goto quick_exit;
        }
    }

    if (!opj_set_decode_area(
            codec, image,
            (OPJ_INT32)parameters.DA_x0,
            (OPJ_INT32)parameters.DA_y0,
            (OPJ_INT32)parameters.DA_x1,
            (OPJ_INT32)parameters.DA_y1)
        )
    {
        fprintf(stderr, "ERROR -> opj_decompress: failed to set the decoded area\n");
        goto quick_exit;
    }

    /* Get the decoded image */
    if (!(opj_decode(codec, stream, image) && opj_end_decompress(codec, stream)))
    {
        fprintf(stderr, "ERROR -> opj_decompress: failed to decode image!\n");
        goto quick_exit;
    }

    for (unsigned int c_index = 0; c_index < image->numcomps; c_index++)
    {
        int width = (int)image->comps[c_index].w;
        int height = (int)image->comps[c_index].h;
        int precision = (int)image->comps[c_index].prec;
        int nr_bytes = ((precision + 7) & -8) / 8;

        printf(
            "Component %u characteristics: %d x %d, %s %d bit, %d byte\n",
            c_index, width, height,
            image->comps[c_index].sgnd == 1 ? "signed" : "unsigned",
            precision,
            nr_bytes
        );

        // Copy image data to the output uint8 numpy array
        // The decoded data type is OPJ_INT32, so we need to convert... ugh!
        int *ptr = image->comps[c_index].data;
        int mask = (1 << precision) - 1;

        union {
            unsigned short val;
            unsigned char vals[2];
        } u16;

        if (precision <= 8)
        {
            // 8-bit signed/unsigned
            for (int row = 0; row < height; row++)
            {
                for (int col = 0; col < width; col++)
                {
                    *out = (unsigned char)(*ptr & mask);
                    out++;
                    ptr++;
                }
            }
        }
        else if (precision <= 16)
        {
            // 16-bit signed/unsigned
            for (int row = 0; row < height; row++)
            {
                for (int col = 0; col < width; col++)
                {
                    u16.val = (unsigned short)(*ptr & mask);
                    *out = u16.vals[0];
                    out++;
                    *out = u16.vals[1];
                    out++;
                    ptr++;
                }
            }
        }
        else
        {
            fprintf(stderr, "ERROR -> More than 16-bits per component not implemented\n");
            goto quick_exit;
        }
    }

    destroy_parameters(&parameters);
    opj_destroy_codec(codec);
    opj_image_destroy(image);
    opj_stream_destroy(stream);

    return 1;

    quick_exit:
        destroy_parameters(&parameters);
        if (codec)
            opj_destroy_codec(codec);
        if (image)
            opj_image_destroy(image);
        if (stream)
            opj_stream_destroy(stream);

        return -1;
}
