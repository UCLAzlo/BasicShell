/*****************************************************************************
Small Shell
Author: Daniel Meirovitch
Date: May 22 2019

Description: Creates a small shell to run on top of existing Unix server.
Has built-in commands, cd/status/exit. Rest of commands work via fork and exec.
*****************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>

#define MAXLINE     2048
#define MAXARGS     512
#define MAXPATH     1000
#define MAXBG       50

/*****************************************************************************
Global Variables + Function Prototypes
*****************************************************************************/
void printIfSIGINT(int lastExitStatus);
void printIfChangeFGMode(int startProcFgMode);
void catchSIGSTP(int signo);
void setSIGCatches();
void setIgnoreSIG(int signo);
void setDefaultSIG(int signo);
void initBackgroundPIDs();
int checkFileRedirection(char** args, char** file, char* fileSymbol);
void performFileRedirection(char* file, int isOutput);
int checkForBackground(char** args, int maxArgs);
void runCD(char** args, int maxArgs);
void runStatus(int lastExitStatus);
void runNonStandard(char** args, int maxArgs, int* lastExitStatus);
void variableExpandPID(char* token, char* newArg);
void printBgExitStatus(pid_t pid, int childExitMethod);
void reapZombies();
char* getInput();
void shellDriver();

struct pidArray
{
    pid_t data[MAXBG];
    int count;
};

struct pidArray backgroundPIDs;
int fgOnlyMode = 0;
int runningFGPid = -1;

/*****************************************************************************
Check for foreground terminated SIGINT
Code cited from class slides
*****************************************************************************/
void printIfSIGINT(int lastExitStatus)
{
    if(WIFSIGNALED(lastExitStatus) != 0)
    {
        int termSignal = WTERMSIG(lastExitStatus);
        if(termSignal == SIGINT)
        {
            printf("terminated by signal %d\n", termSignal);
            fflush(stdout);
        }
    }

}

/*****************************************************************************
If Foreground Only Mode changed while process was running
Print current status message
*****************************************************************************/
void printIfChangeFGMode(int startProcFgMode)
{
    // If mode flipped while a process was running
    if(startProcFgMode != fgOnlyMode)
    {
        //print current status of foreground only mode
        //NOTE conditional is inversed from signal handler
        //because signal handler flips the fgMode variable
        //before this fn can print
        if (!fgOnlyMode)
            printf("Exiting foreground-only mode\n");
        else
            printf("Entering foreground-only mode (& is now ignored)\n");
    }
}

/*****************************************************************************
Signal Handler to catch SIGTSTP from CTRL-Z
*****************************************************************************/
void catchSIGTSTP(int signo)
{
    // Immediately print foreground only message if no running process
    if(runningFGPid == -1)
    {
        //print current status of foreground only mode
        if (fgOnlyMode)
        {
            char* message = "Exiting foreground-only mode\n: ";
            write(STDOUT_FILENO, message, 31);
        }
        else
        {
            char* message = "Entering foreground-only mode (& is now ignored)\n: ";
            write(STDOUT_FILENO, message, 51);
        }
    }
    
    //flip value of fgOnlyMode variable
    fgOnlyMode = !fgOnlyMode;
}

/*****************************************************************************
Initialize Signal Handler Structs
*****************************************************************************/

void setSIGCatches()
{
    //setup SIGTSTP Handler
    //Use SA_RESTART to resume parent after SIGTSTP
    struct sigaction SIGTSTP_action = {{0}};
    SIGTSTP_action.sa_handler = catchSIGTSTP;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);
}

/*****************************************************************************
Tell Process to ignore signal param
*****************************************************************************/

void setIgnoreSIG(int signo)
{
    struct sigaction ignore_action = {{0}};
    ignore_action.sa_handler = SIG_IGN;
    sigaction(signo, &ignore_action, NULL);
}

/*****************************************************************************
Tell Process to use default behavior for signal param
*****************************************************************************/

void setDefaultSIG(int signo)
{
    struct sigaction default_action = {{0}};
    default_action.sa_handler = SIG_DFL;
    sigaction(signo, &default_action, NULL);
}

/*****************************************************************************
Sets the global background PID array to all 0s and count to 0
*****************************************************************************/
void initBackgroundPIDs()
{
    int i;
    for(i = 0; i < MAXBG; i++)
        backgroundPIDs.data[i] = 0;
    backgroundPIDs.count = 0;
}

/*****************************************************************************
Looks through the list of args to see if there is an input/output redirection
< or > symbol
If there is, store the redirection into the file string
Remake the args array to get rid of file redirection symbols
*****************************************************************************/
int checkFileRedirection(char** args, char** file, char* fileSymbol)
{
    //loop through all 512 args, or until reach the end of args
    int i;
    for(i = 0; (i < MAXARGS) && (args[i] != NULL); i++)
    {
        //found input redirection
        //save fileIn string and adjust args array to cover over input redirection
        if(!strcmp(args[i], fileSymbol))
        {
            *file = args[i+1];
            int j;
            for(j = i; (j < MAXARGS-2) && (args[j] != NULL); j++)
                args[j] = args[j+2];
            return 1;
        }
    }

    return 0;
}

/*****************************************************************************
Perform file redirection in a child process using dup2.

Assumes that file entry is not NULL and is called from a child process
Opens file for write if isOutput==1, else opens file for read
If for ouput, redirects file to stdout==1. Else redirects to stdin==0
*****************************************************************************/
void performFileRedirection(char* file, int isOutput)
{
    //open file as read or write depending on whether output or input file
    int FD;
    if(isOutput)
        FD = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    else
        FD = open(file, O_RDONLY);

    //print matching error message if failed to open file
    if(FD == -1)
    {
        if(isOutput)
            fprintf(stderr, "ERROR: opening file for output redirection\n");
        else
            fprintf(stderr, "ERROR: opening file for input redirection\n");
        exit(1);
    }

    //redirect file descriptor, and print error if it occurs
    int result = dup2(FD, isOutput);
    if(result == -1)
    {
        fprintf(stderr, "ERROR: dup2\n");
        exit(1);
    }
}

/*****************************************************************************
Checks to see if process should be run in the background 
(indicated by & argument) and removes & character if found
Returns 1 if true
Returns 0 if false
*****************************************************************************/
int checkForBackground(char** args, int maxArgs)
{
    //check if last arg indicates a background
    if(!strcmp(args[maxArgs - 1], "&"))
    {
        args[maxArgs - 1] = NULL;
        return 1;
    }
    //else not, return false
    return 0;
}

/*****************************************************************************
Parses through a non-built in command. Forks and executes process
*****************************************************************************/
void runNonStandard(char** args, int maxArgs, int* lastExitStatus)
{
    pid_t spawnPid = -5;
    int isBackground = checkForBackground(args, maxArgs);
    char* fileIn = NULL;
    char* fileOut = NULL;

    spawnPid = fork();
    switch (spawnPid)
    {
        //error when forking
        case -1:
            fprintf(stderr, "MAJOR FORK ERROR, ABORT ABORT! \n");
            exit(1);
            break;

        //Child Fork
        case 0:
            
            //Check for input file redirection <
            //If found, update fileIn and perform file redirection with dup2
            //else if it is a background process then redirect to /dev/null
            if(checkFileRedirection(args, &fileIn, "<"))
                performFileRedirection(fileIn, 0);
            else if(isBackground && !fgOnlyMode)
                performFileRedirection("/dev/null", 0);

            //Repeat same as above with output file redirection >
            if(checkFileRedirection(args, &fileOut, ">"))
                performFileRedirection(fileOut, 1);
            else if(isBackground && !fgOnlyMode)
                performFileRedirection("/dev/null", 1);

            //tell all child to ignore SIGTSTP
            setIgnoreSIG(SIGTSTP);

            //tell foreground processes to terminate on SIGINT
            if(!isBackground || fgOnlyMode)
                setDefaultSIG(SIGINT);

            //execute function
            execvp(args[0], args);

            //cleanup if exec failed
            fprintf(stderr, "ERROR: Command could not be found on system\n");
            exit(1);
            break;

        //Parent Fork
        default:
            //if it is a background process, don't hang while waiting
            //save PID for checking later
            if(isBackground && !fgOnlyMode)
            {
                printf("Background pid is %d\n", spawnPid);
                fflush(stdout);
                backgroundPIDs.data[backgroundPIDs.count] = spawnPid;
                backgroundPIDs.count++;
            }
            //else foreground process hang and wait shell prompt until done
            //record running PID for tracking in Signal Handlers
            else
            {
                int startProcFgMode = fgOnlyMode;
                runningFGPid = spawnPid;
                waitpid(spawnPid, lastExitStatus, 0);
                runningFGPid = -1;
                printIfSIGINT(*lastExitStatus);
                printIfChangeFGMode(startProcFgMode);
            }

            break;
    }
}

/*****************************************************************************
Code for the built-in exit command. 
Kills all background PIDs gracefully, waits for them to complete, and then exits
*****************************************************************************/
int runExit()
{
    int i;
    int status;
    for(i = 0; i < backgroundPIDs.count; i++)
    {
        kill(backgroundPIDs.data[i], SIGTERM);
        waitpid(backgroundPIDs.data[i], &status, 0);
    }
    return 1;
}

/*****************************************************************************
Code for the built-in cd command. 
Changes to specified dir or home dir
*****************************************************************************/
void runCD(char** args, int maxArgs)
{
    int cdStatus = 1;
    if(maxArgs > 2)
        printf("ERROR: Too Many Arguments\n");
    else if(args[1] == NULL)
        cdStatus = chdir(getenv("HOME"));
    else
        cdStatus = chdir(args[1]);

   if(cdStatus == -1)
   {
        printf("ERROR: Could not find directory, working directory unchanged\n");
        fflush(stdout);
   }

}

/*****************************************************************************
Code for built-in status command. 
Report exit status/signal of last finished PID
Code largely taken from CS 344 3.1 slides
*****************************************************************************/
void runStatus(int lastExitStatus)
{
    //if -1, print exit status is 0
    if(lastExitStatus == -1)
    {
        printf("No non-standard command has been run, exit status is 0\n");
        fflush(stdout);
    }
    
    //else retrieve actual exit/signal
    else if (WIFEXITED(lastExitStatus))
    {
        int exitStatus = WEXITSTATUS(lastExitStatus);
        printf("Exit status was %d\n", exitStatus);
        fflush(stdout);
    }
    else if(WIFSIGNALED(lastExitStatus) != 0)
    {
        int termSignal = WTERMSIG(lastExitStatus);
        printf("terminated by signal %d\n", termSignal);
        fflush(stdout);
    }
}

/*****************************************************************************
When user enters a $$, expand that into the PID. Do that for all entries in line
*****************************************************************************/
void variableExpandPID(char* line, char* expandedLine)
{
    //expand $$ if any found
    if(strstr(line, "$$"))
    {
        //get pid and convert to string
        char pidStr[6];
        memset(pidStr, '\0', sizeof(pidStr));
        pid_t curPid = getpid();
        snprintf(pidStr, 6, "%d", curPid);

        //loop through string until all $$ are converted
        char* startOfSubStr;
        int subStrFound = 1;
        while((startOfSubStr = strstr(line, "$$")))
        {
            //at location of $$, convert to null /0 to mark end of string for strcpy
            *startOfSubStr = '\0';

            //construct new string based on token up to $$ and then append pid
            if(subStrFound == 1)
                strcpy(expandedLine, line);
            else
                strcat(expandedLine, line); 
            strcat(expandedLine, pidStr);

            //update token to search for next $$
            line = startOfSubStr + 2;
            subStrFound++;
        }
    }
    //else, just use the user input line as is
    else
    {
        strcpy(expandedLine, line);
    }
}

/*****************************************************************************
Based on childExitMethod set by waitPid, print out the exit status
concept taken from CS344 3.1 Slides
*****************************************************************************/
void printBgExitStatus(pid_t pid, int childExitMethod)
{
    printf("Background pid %d is done: ", pid);
    if (WIFEXITED(childExitMethod))
    {
        int exitStatus = WEXITSTATUS(childExitMethod);
        printf("Exit status was %d\n", exitStatus);
        fflush(stdout);
    }
    else if(WIFSIGNALED(childExitMethod) != 0)
    {
        int termSignal = WTERMSIG(childExitMethod);
        printf("terminated by signal %d\n", termSignal);
        fflush(stdout);
    }
}

/*****************************************************************************
Loops through all running background processes, reap any zombies
*****************************************************************************/
void reapZombies()
{
    int i;
    for(i = 0; i < backgroundPIDs.count; i++)
    {
        int childExitMethod = -5;
        //if process completed, print data and remove data from array
        if(waitpid(backgroundPIDs.data[i], &childExitMethod, WNOHANG))
        {
            printBgExitStatus(backgroundPIDs.data[i], childExitMethod);
            backgroundPIDs.data[i] = backgroundPIDs.data[backgroundPIDs.count - 1];
            backgroundPIDs.count--;
            i--;
        }
    }
}

/*****************************************************************************
Collects User Input, mostly taken from Example Code 3.3
https://oregonstate.instructure.com/courses/1719569/pages/3-dot-3-advanced-user-input-with-getline

Has controls for edge cases (blank line, comment line, over max, no entry)
*****************************************************************************/
char* getInput()
{
    int numCharsEntered = -5;
    size_t bufferSize = 0;
    char* lineEntered = NULL;

    while(1)
    {
        //check for any running background process, then collect user input
        reapZombies();
        printf(": ");
        fflush(stdout);
        numCharsEntered = getline(&lineEntered, &bufferSize, stdin);
        //prompt user again if there was an error
        if(numCharsEntered == -1)
            clearerr(stdin);
        //prompt user again for blank line
        else if(numCharsEntered == 1 && !strcmp(lineEntered, "\n"))
            clearerr(stdin);
        //prompt user again if over max char length
        else if(numCharsEntered > MAXLINE)
            clearerr(stdin);
        //prompt user again if it is a comment line (first character is a #)
        else if(lineEntered[0] == '#')
            clearerr(stdin);
        else
            break;
    }

    //Remove ending new line character from User Input and return
    lineEntered[strcspn(lineEntered, "\n")] = '\0';
    return lineEntered;
}

/*****************************************************************************
Creates the interactive shell and drives the program
Loops until user types exit command
*****************************************************************************/
void shellDriver()
{
    int quit = 0;
    int lastExitStatus = -1;
    while(!quit)
    {
        //get user input, and expand all $$ substrings
        char* line = getInput();
        char expandedLine[MAXLINE];
        memset(expandedLine, '\0', sizeof(expandedLine));
        variableExpandPID(line, expandedLine);
        free(line);
        line = NULL;

        //Split the line into cmd and relevant arguments, splits on space
        //First entry is cmd, next entries are arguments
        //mark last argument as NULL
        int argIdx = 1;
        char* args[MAXARGS];

        char* cmd = args[0] = strtok(expandedLine, " ");
        char* token = strtok(NULL, " ");

        //loop through line input to split into tokens
        while(token != NULL)
        {
            args[argIdx] = token;
            argIdx++;
            token = strtok(NULL, " ");
        }
        args[argIdx] = NULL;
        
        //check for a builtin command and run as appropriate
        if(!strcmp(cmd, "exit"))
            quit = runExit();
        else if(!strcmp(cmd, "cd"))
            runCD(args, argIdx);
        else if(!strcmp(cmd, "status"))
            runStatus(lastExitStatus);

        //else it is a non-builtin and needs to be run with fork/exec
        else
            runNonStandard(args, argIdx, &lastExitStatus);
    }
}

/*****************************************************************************
Main Function
*****************************************************************************/
int main()
{
    setSIGCatches();
    setIgnoreSIG(SIGINT);
    initBackgroundPIDs();
    shellDriver();
    return 0;
}