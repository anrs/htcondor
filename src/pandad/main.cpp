#include "condor_common.h"
#include "condor_config.h"
#include "condor_debug.h"
#include "subsystem_info.h"

#include <string>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

#include "PipeBuffer.h"
#include "Queue.h"

#include <curl/curl.h>

#include <vector>
#include <map>

unsigned badCommandCount = 0;
pthread_mutex_t bigGlobalMutex = PTHREAD_MUTEX_INITIALIZER;

void configureSignals();
static void * workerFunction( void * ptr );

// Yes, globals are evil, and yes, I could wrap these and the queue in
// a struct and pass that to thread instead, but why bother?
bool reconfigure = false;
std::string pandaURL;
std::string vomsProxy;
std::string vomsCAPath;
std::string vomsCACert;
int timeout = 0;

int main( int /* argc */, char ** /* argv */ ) {
	configureSignals();

	// This incantation makes dprintf() work.
	set_mySubSystem( "pandad", SUBSYSTEM_TYPE_DAEMON );
	config();
	dprintf_config( "pandad" );
	const char * debug_string = getenv( "DebugLevel" );
	if( debug_string && * debug_string ) {
		set_debug_flags( debug_string, 0 );
	}
	dprintf( D_ALWAYS, "STARTING UP\n" );

	param( pandaURL, "PANDA_URL" );
	param( vomsProxy, "PANDA_VOMS_PROXY" );
	if( pandaURL.empty() || vomsProxy.empty() ) {
		dprintf( D_ALWAYS, "You must define both PANDA_URL and PANDA_VOMS_PROXY for pandad to function.\n" );
		exit( 1 );
	}

	param( vomsCAPath, "PANDA_VOMS_CA_PATH" );
	param( vomsCACert, "PANDA_VOMS_CA_FILE" );
	if( vomsCAPath.empty() && vomsCACert.empty() ) {
		dprintf( D_ALWAYS, "One of PANDA_VOMS_CA_[PATH|CERT] is probably necessary, but neither is set.\n" );
	}

	timeout = param_integer( "PANDA_UPDATE_TIMEOUT" );

	int queueSize = param_integer( "PANDA_QUEUE_SIZE" );


	// curl_global_init() is not thread-safe.
	CURLcode crv = curl_global_init( CURL_GLOBAL_ALL );
	if( crv != 0 ) {
		dprintf( D_ALWAYS, "Failed curl_global_init(), aborting.\n" );
		exit( 1 );
	}


	// To make sure that we're the only thread running, take the big
	// global mutex before creating any others.
	pthread_mutex_lock( & bigGlobalMutex );

	// A locking queue.
	Queue< std::string > queue( queueSize, & bigGlobalMutex );

	// We only need one worker thread.
	pthread_t workerThread;
	int rv = pthread_create( & workerThread, NULL, workerFunction, & queue );
	if( rv != 0 ) {
		dprintf( D_ALWAYS, "Unable to create worker thread (%d), exiting.\n", rv );
		exit( 1 );
	}
	rv = pthread_detach( workerThread );
	if( rv != 0 ) {
		dprintf( D_ALWAYS, "Unable to detach worker thread (%d), exiting.\n", rv );
		exit( 1 );
	}


	std::string * line = NULL;
	PipeBuffer pb( STDIN_FILENO, & bigGlobalMutex );
	for( ;; ) {
		while( (line = pb.getNextLine()) != NULL ) {
			dprintf( D_FULLDEBUG, "getNextLine() = %s\n", line->c_str() );
			queue.enqueue( * line );
			delete line;
		}

		if( pb.isError() || pb.isEOF() ) {
			dprintf( D_ALWAYS, "stdin closed, draining queue...\n" );
			queue.drain();
			dprintf( D_ALWAYS, "stdin closed, ... done.  Exiting.\n" );
			exit( 0 );
		}
	}
} // end main()

#if ! defined( WIN32 )

#include <signal.h>

void quit( int signal ) {
	// POSIX exit() is not signal-safe.
	_exit( signal );
}

void sighup( int signal ) {
	if( signal != SIGHUP ) { return; }
	reconfigure = true;
}

void configureSignals() {
	// Stolen from the GAHPs.
	sigset_t sigSet;
	struct sigaction sigAct;

	sigemptyset( &sigAct.sa_mask );
	sigAct.sa_flags = 0;
	sigAct.sa_handler = quit;
	sigaction( SIGTERM, &sigAct, 0 );
	sigaction( SIGQUIT, &sigAct, 0 );

	sigemptyset( & sigSet );
	sigaddset( &sigSet, SIGTERM );
	sigaddset( &sigSet, SIGQUIT );
	sigaddset( &sigSet, SIGPIPE );
	sigprocmask( SIG_UNBLOCK, & sigSet, NULL );

	// Handle SIGHUP.
	sigemptyset( & sigAct.sa_mask );
	sigAct.sa_flags = 0;
	sigAct.sa_handler = sighup;
	sigaction( SIGHUP, & sigAct, 0 );

	sigemptyset( & sigSet );
	sigaddset( & sigSet, SIGHUP );
	sigprocmask( SIG_UNBLOCK, & sigSet, NULL );
} // end configureSignals()

#else // ! defined( WIN32 )

void configureSignals() { }

#endif

typedef std::map< std::string, std::string > Command;
typedef std::map< std::string, Command > CommandSet;
CommandSet addCommands, updateCommands, removeCommands;

std::string & doValueCleanup( std::string & value ) {
	//
	// We're sent unparse()d ClassAd values, but we need to send JSON.
	//
	if( value[0] == '"' ) {
		if( value[ value.length() - 1 ] != '"' ) {
			dprintf( D_ALWAYS, "String value '%s' lacks trailing \", appending.\n", value.c_str() );
			value += "\"";
		}

		// All ClassAd escapes are legal JSON escapes, except for single
		// quote and the octal escapes.  The former shouldn't be escaped
		// and the latter must be converted into hex.  All ClassAd bytes
		// are between U+0020 and U+007E (with U+005C = '\' already being
		// escaped by unparse()), so they pass through unchanged.

		size_t index = 1;
		while( index < value.length() - 1 ) {
			if( value[ index ] != '\\' ) { index += 1; continue; }
			char c = value[ index + 1 ];
			// Ignore escaped escapes.
			if( c == '\\' ) { index += 2; continue; }
			// Convert escaped single quotes.
			if( c == '\'' ) { value.replace( index, 2, "'" ); index += 1; continue; }
			// Convert octal escapes.  The job queue may never generate these.
			if( ! isdigit( c ) ) { index += 2; continue; }

			const char * left = value.c_str();
			left = left + index + 1;
			char * right;
			unsigned long octal = strtoul( left, & right, 8 );
			if( right == left) {
				dprintf( D_ALWAYS, "Detected invalid octal escape, escaping.\n" );
				value.replace( index, 2, "\\\\" );
				index += 2;
				continue;
			}
			std::string cStr;
			formatstr( cStr, "\\u%04lX", octal );
			value.replace( index, right - left + 1, cStr.c_str() );
			index += right - left + 1;
		}
	} else if( isdigit( value[0] ) ) {
		// Sometimes integers are represented as floats with lots of trailing
		// zeroes, which can cause some parsers grief.  Convert them.
		size_t point = value.find( '.' );
		if( point == std::string::npos ) { return value; }
		for( size_t i = point + 1; i < value.length(); ++i ) {
			if( value[i] != '0' ) { return value; }
		}

		// If we get here, then everything after the point must have been 0,
		// so we can safely truncate the string.
		value.erase( point, std::string::npos );
	} else {
		// A literal value.
		if( value == "null" ) { return value; }
		else if( value == "false" ) { return value; }
		else if( value == "true" ) { return value; }
		else {
			// JSON does not permit any other literal.
			dprintf( D_ALWAYS, "Converting illegal literal '%s' into string.\n", value.c_str() );
			value = '"' + value + '"';
		}
	}
	return value;
}

void constructCommand( const std::string & line ) {
	//
	// Before parsing the command, make sure that it wasn't mangled by
	// an aborted partial write.  All writes from the schedd are of the
	// form '\v[ADD|UPDATE|REMOVE] <arg>+\n'.  We know we have the newline
	// because we feed the queue line-by-line.  If we find only one \v in
	// the entire line, at the beginning of the line, then the write wasn't
	// aborted, and it should be safe to parse.
	//
	size_t firstVerticalTab = line.find( '\v' );
	if( firstVerticalTab != 0 ) {
		++badCommandCount;
		return;
	}

	size_t nextSpace;
	size_t currentSpace = 0;
	std::vector< std::string > argv;
	do {
		// We know that the last line in the string is an newline, so
		// currentSpace can never be the last character; thus currentSpace + 1
		// will always be a valid index.
		nextSpace = line.find( ' ', currentSpace + 1 );
		argv.push_back( line.substr( currentSpace + 1, nextSpace - (currentSpace + 1) ) );
		currentSpace = nextSpace;
	} while( currentSpace != std::string::npos );

	std::string command = argv[0];
	std::string globalJobID = argv[1];
	if( command == "ADD" ) {
		Command & c = addCommands[ globalJobID ];
		c[ "globaljobid" ] = globalJobID;
		c[ "condorid" ] = argv[2];
	} else if( command == "UPDATE" ) {
		Command & c = updateCommands[ globalJobID ];
		c[ "globaljobid" ] = globalJobID;
		// Panda should redefine their schema so that 'submitted'
		// is TIMESTAMP, not DATETIME. *sigh*  Include the quotes
		// because TIMESTAMP is a string to JSON.
		if( argv[2] == "submitted" ) {
			time_t submitTime = atol( argv[3].c_str() );
			char buffer[] = "\"YYYY-MM-DD HH:MM:SS\"";
			struct tm * ts = localtime( & submitTime );
			strftime( buffer, sizeof( buffer ), "\"%Y-%m-%d %H:%M:%S\"", ts );
			argv[3] = buffer;
		}
		c[ argv[2] ] = doValueCleanup( argv[3] );
	} else if( command == "REMOVE" ) {
		Command & c = removeCommands[ globalJobID ];
		c[ "globaljobid" ] = globalJobID;
	} else {
		dprintf( D_ALWAYS, "workerFunction() ignoring unknown command (%s).\n", command.c_str() );
	}
} // end constructCommand()

std::string generatePostString( const CommandSet & commandSet ) {
	std::string postFieldString = "[ ";
	CommandSet::const_iterator i = commandSet.begin();
	for( ; i != commandSet.end(); ++i ) {
		const Command & c = i->second;

		postFieldString += "{ ";

		Command::const_iterator j = c.begin();
		for( ; j != c.end(); ++j ) {
			postFieldString += "\"" + j->first + "\": ";
			postFieldString += j->second + ", ";
		}

		// TO DO: Determine what, if anything, 'wmsid' ought to be; try to
		// convince the PANDA people that it's not required?
		postFieldString += "\"wmsid\": 1004";
		postFieldString += " }, ";
	}
	// Remove trailing ', '.
	postFieldString.erase( postFieldString.length() - 2 );
	postFieldString += " ]";

	return postFieldString;
} // end generatePostString()

#define SET_REQUIRED_CURL_OPTION( A, B, C ) { \
	CURLcode rv##B = curl_easy_setopt( A, B, C ); \
	if( rv##B != CURLE_OK ) { \
		dprintf( D_ALWAYS, "curl_easy_setopt( %s ) failed (%d): '%s', aborting.\n", \
			#B, rv##B, curl_easy_strerror( rv##B ) ); \
		exit( 1 ); \
	} \
}

// TO DO: This should either be removed, or, if the information is of
// general interest, changed to obtain the file name and minimum interval
// from the config system (param() and its table).
void updateStatisticsLog( unsigned queueFullCount ) {
	static time_t lastLogTime = 0;

	time_t now = time( NULL );
	if( now - 60 > lastLogTime ) {
		lastLogTime = now;
		int fd = open( "/tmp/pandaStatisticsLog", O_CREAT | O_TRUNC | O_WRONLY | O_NONBLOCK, 00600 );
		FILE * psl = fdopen( fd, "w" );
		if( fd != -1 ) {
		fprintf( psl, "queueFullCount: %u\n", queueFullCount );
		fprintf( psl, "badCommandCount: %u\n", badCommandCount );
		fclose( psl );
		}
	}
}

bool sendCommands( CURL * curl, const std::string & url, const CommandSet & commandSet, char * curlErrorBuffer, unsigned long & responseCode, unsigned queueFullCount ) {
	if( commandSet.size() == 0 ) { return false; }
	std::string postString = generatePostString( commandSet );

	dprintf( D_FULLDEBUG, "Sending commands to URL '%s'.\n", url.c_str() );
	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_URL, url.c_str() );
	dprintf( D_FULLDEBUG, "Sending POST '%s'.\n", postString.c_str() );
	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_POSTFIELDS, postString.c_str() );

	pthread_mutex_unlock( & bigGlobalMutex );
	updateStatisticsLog( queueFullCount );
	CURLcode rv = curl_easy_perform( curl );
	pthread_mutex_lock( & bigGlobalMutex );

	if( rv != CURLE_OK ) {
		dprintf( D_ALWAYS, "Ignoring curl_easy_perform() error (%d): '%s'\n", rv, curl_easy_strerror( rv ) );
		dprintf( D_ALWAYS, "curlErrorBuffer = '%s'\n", curlErrorBuffer );

		// It would be nice, for performance monitoring, to dump some more
		// information about the timed-out request here.

		return false;
	}

	rv = curl_easy_getinfo( curl, CURLINFO_RESPONSE_CODE, & responseCode );
	if( rv != CURLE_OK ) {
		dprintf( D_ALWAYS, "Ignoring curl_easy_getinfo() error (%d): '%s'\n", rv, curl_easy_strerror( rv ) );
		dprintf( D_ALWAYS, "curlErrorBuffer = '%s'\n", curlErrorBuffer );
		return false;
	}

	return true;
} // end sendCommands()

// Required for debugging.  Stolen from ec2_gahp/amazonCommands.cpp.
size_t appendToString( const void * ptr, size_t size, size_t nmemb, void * str ) {
	if( size == 0 || nmemb == 0 ) { return 0; }

	std::string source( (const char *)ptr, size * nmemb );
	std::string * ssptr = (std::string *)str;
	ssptr->append( source );

	return (size * nmemb);
}

static void * workerFunction( void * ptr ) {
	Queue< std::string > * queue = (Queue< std::string > *)ptr;
	pthread_mutex_lock( & bigGlobalMutex );

	// Do the command-independent curl set-up.
	CURL * curl = curl_easy_init();
	if( curl == NULL ) {
		dprintf( D_ALWAYS, "Failed curl_easy_init(), aborting.\n" );
		exit( 1 );
	}

	char curlErrorBuffer[CURL_ERROR_SIZE];
	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_ERRORBUFFER, curlErrorBuffer );
	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_NOPROGRESS, 1 );
	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_SSLCERT, vomsProxy.c_str() );
	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_SSLKEY, vomsProxy.c_str() );
  	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_POST, 1 );
	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_NOSIGNAL, 1 );
	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_TIMEOUT, timeout );

	std::string resultString;
	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_WRITEFUNCTION, & appendToString );
	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_WRITEDATA, & resultString );

	// No, this makes no sense.  Yes, it's how their API works.
	struct curl_slist * headers = NULL;
	headers = curl_slist_append( headers, "Content-Type: application/json" );
	SET_REQUIRED_CURL_OPTION( curl, CURLOPT_HTTPHEADER, headers );

	if( ! vomsCAPath.empty() ) {
		SET_REQUIRED_CURL_OPTION( curl, CURLOPT_CAPATH, vomsCAPath.c_str() );
	}
	if( ! vomsCACert.empty() ) {
		SET_REQUIRED_CURL_OPTION( curl, CURLOPT_CAINFO, vomsCACert.c_str() );
	}

	// The trailing slash is required.
	std::string API = pandaURL + "/api-auth/htcondorapi";
	std::string addJob = API + "/addjob/";
	std::string updateJob = API + "/updatejob/";
	std::string removeJob = API + "/removejob/";


	for( ; ; ) {
		//
		// Wait for the queue to fill up.  By sleeping instead of blocking,
		// we give the other thread a chance to insert more than one read
		// into the queue at a time.
		//
		while( queue->empty() ) {
			pthread_mutex_unlock( & bigGlobalMutex );
			sleep( 1 );
			pthread_mutex_lock( & bigGlobalMutex );
		}

		//
		// Empty the qeueue.  We could easily add a limite here to smooth
		// out bursty schedd activity (e.g., 'queue 1000').
		//
		while( ! queue->empty() ) {
			constructCommand( queue->dequeue() );
		}

		//
		// It's (presently) safe to resize the queue at any time, since it's
		// a non-destructive operation, but since the could change, do it
		// now, while we know the queue is empty.
		//
		if( reconfigure ) {
			config();
			unsigned newSize = param_integer( "PANDA_QUEUE_SIZE" );
			dprintf( D_ALWAYS, "Changing queue to size %u on SIGHUP.\n", newSize );
			queue->resize( newSize );
			reconfigure = false;
		}

		// Send all of the add commands.
		unsigned long responseCode = 0;
		bool rc = sendCommands( curl, addJob, addCommands, curlErrorBuffer, responseCode, queue->queueFullCount );
		if( rc ) {
			dprintf( D_FULLDEBUG, "addJob() response code was %ld.\n", responseCode );
			if( responseCode != 201 ) {
				dprintf( D_ALWAYS, "addJob() ignoring non-OK (%ld) response '%s'.\n", responseCode, resultString.c_str() );
			}
		}

		// Send all the update commands.
		rc = sendCommands( curl, updateJob, updateCommands, curlErrorBuffer, responseCode, queue->queueFullCount );
		if( rc ) {
			dprintf( D_FULLDEBUG, "updateJob() response code was %ld.\n", responseCode );
			if( responseCode != 202 ) {
				dprintf( D_ALWAYS, "updateJob() ignoring non-OK (%ld) response '%s'.\n", responseCode, resultString.c_str() );
			}
		}

		// Send all the remove commands.
		rc = sendCommands( curl, removeJob, removeCommands, curlErrorBuffer, responseCode, queue->queueFullCount );
		if( rc ) {
			dprintf( D_FULLDEBUG, "removeJob() response code was %ld.\n", responseCode );
			if( responseCode != 202 ) {
				dprintf( D_ALWAYS, "removeJob() ignoring non-OK (%ld) response '%s'.\n", responseCode, resultString.c_str() );
			}
		}

		// Clear the batched commands.
		addCommands.clear();
		updateCommands.clear();
		removeCommands.clear();
	}


	curl_easy_cleanup( curl ); curl = NULL;
	curl_slist_free_all( headers ); headers = NULL;
	return NULL;
} // end workerFunction()