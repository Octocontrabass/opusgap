#include <ctype.h>
#include <opusenc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// this is definitely too short for full paths
#define MAX_NAME_LEN 0x100

// trim leading/trailing whitespace
static char * trim( char * buffer )
{
	size_t i = strlen( buffer );
	// short-circuit to avoid evaluating buffer[-1]
	while( i && isspace( buffer[ i - 1 ] ) )
	{
		buffer[ --i ] = '\0';
	}
	while( isspace( buffer[0] ) )
	{
		buffer++;
	}
	return buffer;
}

// validate WAV header and extract parameters necessary for decoding.
// very stupid, but good enough for me.
static bool prepare( FILE * restrict wavfile, uint_fast16_t * restrict channels, uint_fast32_t * restrict rate, uint_fast16_t * restrict depth, size_t * restrict samples )
{
	uint8_t buffer[0x2c];
	if( !fread( buffer, 0x2c, 1, wavfile ) ) return false;
	if( memcmp( buffer, "RIFF", 4 ) ) return false;
	if( memcmp( buffer+0x08, "WAVEfmt ", 8 ) ) return false;
	uint_fast32_t fmtsize = buffer[0x10] | (buffer[0x11] << 8) | (buffer[0x12] << 16) | (buffer[0x13] << 24);
	if( fmtsize != 0x10 ) return false; // no extended headers
	uint_fast16_t format = buffer[0x14] | (buffer[0x15] << 8);
	*channels = buffer[0x16] | (buffer[0x17] << 8);
	*rate = buffer[0x18] | (buffer[0x19] << 8) | (buffer[0x1a] << 16) | (buffer[0x1b] << 24);
	*depth = buffer[0x22] | (buffer[0x23] << 8);
	if( memcmp( buffer+0x24, "data", 4 ) ) return false;
	size_t size = buffer[0x28] | (buffer[0x29] << 8) | (buffer[0x2a] << 16) | (buffer[0x2b] << 24); // location depends on header size
	if( format != 1 ) return false; // only PCM
	if( *channels < 1 || *channels > 2 ) return false; // only mono or stereo
	if( *depth != 16 ) return false; // only 16-bit
	*samples = size / *channels / (*depth / 8);
	return true;
}

int main( int argc, char * * argv )
{
	char namebuffer[MAX_NAME_LEN];
	if( !fgets( namebuffer, MAX_NAME_LEN - strlen( ".opus" ), stdin ) )
	{
		fprintf( stderr, "There aren't any file names?\n" );
		return EXIT_FAILURE;
	}
	char * name = trim( namebuffer );
	FILE * wavfile = fopen( name, "rb" );
	if( !wavfile )
	{
		fprintf( stderr, "Failed to open file: %s\n", name );
		return EXIT_FAILURE;
	}
	uint_fast16_t channels;
	uint_fast32_t rate;
	uint_fast16_t current_depth;
	size_t current_samples;
	if( !prepare( wavfile, &channels, &rate, &current_depth, &current_samples ) )
	{
		fprintf( stderr, "Unsupported format: %s\n", name );
		fclose( wavfile );
		return EXIT_FAILURE;
	}
	OggOpusComments * comments = ope_comments_create();
	if( !comments )
	{
		fprintf( stderr, "Out of memory allocating comments? Somehow??\n" );
		fclose( wavfile );
		return EXIT_FAILURE;
	}
	memcpy( name+strlen( name ), ".opus", strlen( ".opus" )+1 );
	OggOpusEnc * encoder = ope_encoder_create_file( name, comments, rate, channels, 0, NULL );
	if( !encoder )
	{
		fprintf( stderr, "Failed to open encoder: %s\n", name );
		ope_comments_destroy( comments );
		fclose( wavfile );
		return EXIT_FAILURE;
	}
	int retval = EXIT_SUCCESS;
	while( 1 )
	{
		uint8_t * wavdata = malloc( current_samples * channels * (current_depth / 8) );
		float * data = malloc( current_samples * channels * sizeof( float ) );
		if( !wavdata || !data )
		{
			fprintf( stderr, "Out of memory.\n" );
			if( wavdata ) free( wavdata );
			if( data ) free( data );
			fclose( wavfile );
			retval = EXIT_FAILURE;
			break;
		}
		if( !fread( wavdata, current_samples * channels * (current_depth / 8), 1, wavfile ) )
		{
			fprintf( stderr, "Error reading input file.\n" );
			free( wavdata );
			free( data );
			fclose( wavfile );
			retval = EXIT_FAILURE;
			break;
		}
		fclose( wavfile );
		
		for( size_t i = 0; i < (current_samples * channels); i++ )
		{
			data[i] = ((int16_t)(wavdata[i * 2] | (wavdata[i * 2 + 1] << 8))) / 32768.f;
		}
		
		free( wavdata );
		if( ope_encoder_ctl( encoder, OPUS_SET_LSB_DEPTH( 16 ) ) != OPE_OK )
		{
			fprintf( stderr, "Failed to set LSB_DEPTH, continuing anyway...\n" );
		}
		if( ope_encoder_write_float( encoder, data, current_samples ) != OPE_OK )
		{
			fprintf( stderr, "Failed to encode file: %s\n", name );
			free( data );
			retval = EXIT_FAILURE;
			break;
		}
		free( data );
		if( !fgets( namebuffer, MAX_NAME_LEN - strlen( ".opus" ), stdin ) ) break;
		name = trim( namebuffer );
		wavfile = fopen( name, "rb" );
		if( !wavfile )
		{
			fprintf( stderr, "Failed to open file: %s\n", name );
			retval = EXIT_FAILURE;
			break;
		}
		uint_fast16_t new_channels;
		uint_fast32_t new_rate;
		if( !prepare( wavfile, &new_channels, &new_rate, &current_depth, &current_samples ) )
		{
			fprintf( stderr, "Unsupported format: %s\n", name );
			fclose( wavfile );
			retval = EXIT_FAILURE;
			break;
		}
		if( new_channels != channels || new_rate != rate )
		{
			fprintf( stderr, "You can't change the sample rate or number of channels!\n" );
			fclose( wavfile );
			retval = EXIT_FAILURE;
			break;
		}
		memcpy( name+strlen( name ), ".opus", strlen( ".opus" )+1 );
		if( ope_encoder_continue_new_file( encoder, name, comments ) != OPE_OK )
		{
			fprintf( stderr, "Failed to continue encoder: %s\n", name );
			fclose( wavfile );
			retval = EXIT_FAILURE;
			break;
		}
	}
	ope_encoder_drain( encoder );
	ope_encoder_destroy( encoder );
	ope_comments_destroy( comments );
	return retval;
}
