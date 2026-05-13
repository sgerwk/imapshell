#!/bin/bash
#
# mechanism:
#	- create a project on the web site
#	- create a client for the project on the web site
#	- the user grants access to email to client

# argument

[ "$1" = -q ] && VERBOSE=false && QUIET='-s' && shift 1
[ $# -lt 1 ] && echo "email missing" && exit 1
MAIL="$1"



# directories

CONFIG=$HOME/.config/imapshell/$MAIL
TMPDIR=/run/user/$UID
[ -d "$TMPDIR" ] || TMPDIR=/tmp

mkdir -p $CONFIG



# client_id and client_secret

CLIENT=$(compgen -G "$CONFIG/client_*.json" | head -1)

if [ "$CLIENT" = '' ];
then

	cat <<-!

	account not configured

	either copy "$CONFIG/" for an existing account

	OR

	configure account:
	    - https://console.developers.google.com/
	    - register project 'imapshell'
	    - register client OAuth2 'imapshell' (in: Credentials)
	      configure: internal, if possible
	      configure: type desktop app
	    - download json file with client_id and client_secret
	    - enable gmail scope https://mail.google.com/
	      (in: imapshell client, Data Access)
	    - if client is external, add email to users (in: Public)
	      (in: Verification center, View audience configuration)

	copy the json file in $CONFIG/ and run this script again

	!

	exit 2

fi

CLIENT_ID=$(cut -d: -f2- "$CLIENT" | tr ',' '\n' | grep client_id | cut -d'"' -f4)
CLIENT_SECRET=$(cut -d: -f2- "$CLIENT" | tr ',' '\n' | grep client_secret | cut -d'"' -f4)

$VERBOSE true && echo $CLIENT_ID
$VERBOSE true && echo $CLIENT_SECRET



# access_token, refresh_token

if [ ! -f "$CONFIG/refresh_token" ];
then

	REDIRECT_URI="http://localhost:8080"
	RESPONSE_TYPE="code"
	SCOPE="https://mail.google.com/"
	PROMPT=consent
	# the code challenge is an arbitrary string of 43-128 characters
	CODE_CHALLENGE=abcdefghabcdefghabcdefghabcdefghabcdefghabcdefghabcdefghabcdefghabcdefgh
	CODE_CHALLENGE_METHOD=plain
	URL="https://accounts.google.com/o/oauth2/v2/auth?client_id=$CLIENT_ID&redirect_uri=$REDIRECT_URI&response_type=$RESPONSE_TYPE&scope=$SCOPE&code_challenge=$CODE_CHALLENGE&code_challenge_method=$CODE_CHALLENGE_METHOD"

	cat <<-!

	authorize access to email
	    - open following url in browser
		$URL
	    - grant access
	    - copy the no-response redirected url

	!

	echo -e 'HTTP/1.1 200 OK\nContent-Type: text/html\nContent-Length: 3\nConnection: close\n\nok\n' | \
	netcat -l -p 8080 > $TMPDIR/redirect.txt
	REDIRECT=$(head -1 $TMPDIR/redirect.txt)
	$VERBOSE true && echo REDIRECT=$REDIRECT

	CODE=$(echo "$REDIRECT" | cut -d' ' -f2 |  tr '&' '\n' | grep code= | cut -d= -f2)
	SCOPE=$(echo "$REDIRECT" | cut -d' ' -f2 | tr '&' '\n' | grep scope= | cut -d= -f2)
	DATA="code=$CODE&client_id=$CLIENT_ID&client_secret=$CLIENT_SECRET&redirect_uri=$REDIRECT_URI&grant_type=authorization_code&code_verifier=$CODE_CHALLENGE"

	curl --request POST --data "$DATA" -o $TMPDIR/tokens.json $QUIET \
		https://accounts.google.com/o/oauth2/token

	ACCESS_TOKEN=$(grep "access_token" $TMPDIR/tokens.json | cut -d'"' -f4)
	REFRESH_TOKEN=$(grep "refresh_token" $TMPDIR/tokens.json | cut -d'"' -f4)

	echo $ACCESS_TOKEN > $CONFIG/access_token
	echo $REFRESH_TOKEN > $CONFIG/refresh_token
	chmod 600 $CONFIG/access_token $CONFIG/refresh_token

fi



# status of token

ACCESS_TOKEN=$(<$CONFIG/access_token)
$VERBOSE true && echo $ACCESS_TOKEN

curl -o $TMPDIR/status.json $QUIET \
	"https://www.googleapis.com/oauth2/v1/tokeninfo?access_token=$ACCESS_TOKEN"
$VERBOSE true && echo "status of current access token"
$VERBOSE true && cat $TMPDIR/status.json
$VERBOSE true && echo



# refresh if needed

if grep -q error $TMPDIR/status.json;
then

	REFRESH_TOKEN=$(<$CONFIG/refresh_token)
	$VERBOSE true && echo $REFRESH_TOKEN

	DATA="client_id=$CLIENT_ID&client_secret=$CLIENT_SECRET&refresh_token=$REFRESH_TOKEN&grant_type=refresh_token"

	curl --request POST --data "$DATA" -o $TMPDIR/refresh.json $QUIET \
		https://accounts.google.com/o/oauth2/token
	$VERBOSE true && echo "result of refreshing the access token"
	$VERBOSE true && cat $TMPDIR/refresh.json
	$VERBOSE true && echo

	ACCESS_TOKEN=$(grep "access_token" $TMPDIR/refresh.json | cut -d'"' -f4)
	echo $ACCESS_TOKEN > $CONFIG/access_token
	chmod 600 $CONFIG/access_token

fi

$VERBOSE true && echo $ACCESS_TOKEN


# use token in imap and smtp

SASL="user=$MAIL\001auth=Bearer $ACCESS_TOKEN\001\001"
AUTH=$(echo -ne "$SASL" | base64 -w 0)

echo -n $AUTH > $CONFIG/sasl
chmod 600 $CONFIG/sasl

