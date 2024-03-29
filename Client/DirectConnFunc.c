#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h> //inet_addr

// Custom Header Files
#include "../Externals.h"
#include "../colour.h"
#include "./Headers.h"
#include "./Hash.h"
#include "./ErrorCodes.h"

void Rcmd(char* arg, int ServerSockfd)
{
    if(CheckNull(arg, ErrorMsg("NULL Argument\nUSAGE: READ <Path>", CMD_ERROR_INVALID_ARGUMENTS)))
    {
        fprintf(Clientlog, "[-]Rcmd: Invalid Argument [Time Stamp: %f]\n", GetCurrTime(Clock));
        return;
    }

    arg = strtok(arg, " \t\n");
    if(strtok(NULL, " \t\n") != NULL)
    {
        fprintf(Clientlog, "[-]Rcmd: Invalid Argument Count [Time Stamp: %f]\n", GetCurrTime(Clock));
        return;
    }

    char* path = arg;
    fprintf(Clientlog, "[+]Rcmd: Reading Path %s [Time Stamp: %f]\n", path, GetCurrTime(Clock));

    REQUEST_STRUCT req_struct;
    REQUEST_STRUCT* req = &req_struct;
    memset(req, 0, sizeof(REQUEST_STRUCT));

    req->iRequestOperation = CMD_READ;
    req->iRequestClientID = iClientID;
    strncpy(req->sRequestPath, path, MAX_BUFFER_SIZE);
    // req->iRequestFlags = 0;

    int iBytesSent = send(ServerSockfd, req, sizeof(REQUEST_STRUCT), 0);

    if(iBytesSent != sizeof(REQUEST_STRUCT))
    {
        char* Msg = ErrorMsg("Failed to send request to server", CMD_ERROR_SEND_FAILED);
        printf(RED"%s\n"reset, Msg);
        fprintf(Clientlog, "[-]Rcmd: Failed to send request [Time Stamp: %f]\n", GetCurrTime(Clock));
        free(Msg);
        return;
    }
    
    RESPONSE_STRUCT res_struct;
    RESPONSE_STRUCT* res = &res_struct;
    memset(res, 0, sizeof(RESPONSE_STRUCT));

    int iBytesRecv = recv(ServerSockfd, res, sizeof(RESPONSE_STRUCT), 0);
    if(iBytesRecv != sizeof(RESPONSE_STRUCT))
    {
        char* Msg = ErrorMsg("Failed to receive response from server", CMD_ERROR_RECV_FAILED);
        printf(RED"%s"reset, Msg);
        fprintf(Clientlog, "[-]Rcmd: Failed to receive response [Time Stamp: %f]\n", GetCurrTime(Clock));
        return;
    }
    
    if(res->iResponseFlags == RESPONSE_FLAG_FAILURE )
    {
        char* Msg = ErrorMsg("Failed to read file", res->iResponseErrorCode);
        printf(RED"%s\n"reset, Msg);
        fprintf(Clientlog, "[-]Rcmd: Failed to read file [Time Stamp: %f]\n", GetCurrTime(Clock));
        free(Msg);
        return;
    }
    else if(res->iResponseFlags == BACKUP_RESPONSE)
    {
        printf(YEL"Corresponding Storage Server is down. Trying to read from backup server\n"reset);
        fprintf(Clientlog, "[+]Rcmd: Corresponding Storage Server is down. Trying to read from backup server [Time Stamp: %f]\n", GetCurrTime(Clock));

        // Modify the path to the backup path
        memset(req->sRequestPath, 0, sizeof(req->sRequestPath));
        snprintf(req->sRequestPath, MAX_BUFFER_SIZE, "./backup%s", path);
    }
    // The response data is the IP and Port of the storage server serving the file seperated by a space
    char* ip = strtok(res->sResponseData, " ");
    char* port = strtok(NULL, " ");

    // Check if Packet corresponds to error
    if(res->iResponseFlags == RESPONSE_FLAG_FAILURE)
    {
        char* Msg = ErrorMsg("Failed to read file", res->iResponseErrorCode);
        printf(RED"%s\n"reset, Msg);
        fprintf(Clientlog, "[-]Rcmd: Failed to read file [Time Stamp: %f]\n", GetCurrTime(Clock));
        free(Msg);
        return;
    }

    // Check if  IP and Port are valid
    if(CheckNull(ip, ErrorMsg("Invalid IP received from server", CMD_ERROR_INVALID_RECV_VALUE)))
    {
        fprintf(Clientlog, "[-]Rcmd: Invalid IP received from server [Time Stamp: %f]\n", GetCurrTime(Clock));
        return;
    }
    else if(CheckNull(port, ErrorMsg("Invalid Port received from server", CMD_ERROR_INVALID_RECV_VALUE)))
    {
        fprintf(Clientlog, "[-]Rcmd: Invalid Port received from server [Time Stamp: %f]\n", GetCurrTime(Clock));
        return;
    }

    // Connect to the storage server
    int StorageSockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(CheckError(StorageSockfd, ErrorMsg("Failed to create socket", CMD_ERROR_SOCKET_FAILED)))
    {
        fprintf(Clientlog, "[-]Rcmd: Failed to create socket [Time Stamp: %f]\n", GetCurrTime(Clock));
        return;
    }

    struct sockaddr_in StorageServer;
    memset(&StorageServer, 0, sizeof(StorageServer));
    StorageServer.sin_family = AF_INET;
    StorageServer.sin_addr.s_addr = inet_addr(ip);
    StorageServer.sin_port = htons(atoi(port));
    memset(StorageServer.sin_zero, '\0', sizeof(StorageServer.sin_zero));

    int iConnectStatus = connect(StorageSockfd, (struct sockaddr *)&StorageServer, sizeof(StorageServer));
    if(CheckError(iConnectStatus, ErrorMsg("Failed to connect to storage server", CMD_ERROR_CONNECT_FAILED)))
    {
        fprintf(Clientlog, "[-]Rcmd: Failed to connect to storage server [Time Stamp: %f]\n", GetCurrTime(Clock));
        return;
    }

    // Send the request to the storage server
    iBytesSent = send(StorageSockfd, req, sizeof(REQUEST_STRUCT), 0);
    if(iBytesSent != sizeof(REQUEST_STRUCT))
    {
        char* Msg = ErrorMsg("Failed to send request to storage server", CMD_ERROR_SEND_FAILED);
        printf(RED"%s\n"reset, Msg);
        fprintf(Clientlog, "[-]Rcmd: Failed to send request to storage server [Time Stamp: %f]\n", GetCurrTime(Clock));
        free(Msg);
        return;
    }
    // Receive stop sequence from server
    char stop[MAX_BUFFER_SIZE];
    iBytesRecv = recv(StorageSockfd, stop, MAX_BUFFER_SIZE, 0);
    if(CheckError(iBytesRecv, ErrorMsg("Failed to receive stop sequence from storage server", CMD_ERROR_RECV_FAILED)))
    {
        fprintf(Clientlog, "[-]Rcmd: Failed to receive stop sequence from storage server [Time Stamp: %f]\n", GetCurrTime(Clock));
        return;
    }
    // printf("Stop Sequence: %s\n", stop);

    // Receive the File from the storage server in chunks of MAX_BUFFER_SIZE until the server sends a chunk of size less than MAX_BUFFER_SIZE
    long long int FileSize = 0;
    printf("File Contents:\n"MAG"----------------------------------------\n");
    while(1)
    {
        char buffer[MAX_BUFFER_SIZE];
        memset(buffer, 0, MAX_BUFFER_SIZE);

        iBytesRecv = recv(StorageSockfd, buffer, MAX_BUFFER_SIZE, 0);
        if(CheckError(iBytesRecv, ErrorMsg("Failed to receive file from storage server", CMD_ERROR_RECV_FAILED)))
        {
            fprintf(Clientlog, "[-]Rcmd: Failed to receive file from storage server [Time Stamp: %f]\n", GetCurrTime(Clock));
            return;
        }

        // Check if the server has sent the stop sequence
        if(strncmp(buffer, stop, MAX_BUFFER_SIZE) == 0)
        {
            break;
        }

        // print the recieved data
        printf("%s", buffer);
        FileSize += strlen(buffer);

    }
    
    printf("\n----------------------------------------\n"reset);
    printf("Read Bytes: %lld Bytes\n", FileSize);
    // Receive the response from the storage server
    iBytesRecv = recv(StorageSockfd, res, sizeof(RESPONSE_STRUCT), 0);
    if(iBytesRecv != sizeof(RESPONSE_STRUCT))
    {
        char* Msg = ErrorMsg("Failed to receive response from storage server", CMD_ERROR_RECV_FAILED);
        printf(RED"%s\n"reset, Msg);
        fprintf(Clientlog, "[-]Rcmd: Failed to receive response from storage server [Time Stamp: %f]\n", GetCurrTime(Clock));
        free(Msg);
        return;
    }

    if(res->iResponseFlags == RESPONSE_FLAG_FAILURE)
    {
        char* Msg = ErrorMsg("Failed to read file from storage server", res->iResponseErrorCode);
        printf(RED"%s\n"reset, Msg);
        fprintf(Clientlog, "[-]Rcmd: Failed to read file from storage server [Time Stamp: %f]\n", GetCurrTime(Clock));
        free(Msg);
        return;
    }

    // log the response
    fprintf(Clientlog, "[+]Rcmd: Server Response: %s [Time Stamp: %f]\n", res->sResponseData, GetCurrTime(Clock));

    // Close the socket
    close(StorageSockfd);
    fprintf(Clientlog, "[+]Rcmd: Successfully read file [Time Stamp: %f]\n", GetCurrTime(Clock));
    return;
}
void Wcmd(char* arg, int ServerSockfd)
{
    return;
}
void Icmd(char* arg, int ServerSockfd)
{
    return;
}