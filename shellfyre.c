/**
 * @file shellfyre.c
 * @author Ata Sayın(ID:64437), Sinan Keser (ID:76982)
 * @brief Comp304 project which similates an Unix-style 
 * operating system shell.
 * @date 2022-04-03
 * 
 * @copyright Copyright (c) 2022
 * 
 */

#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h> //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/mman.h>
const char *sysname = "shellfyre";

#define MAX_SIZE 20
#define PATH_MAX 100
#define MAX_HISTROY_SIZE 10

#define FILE_NUMBER 22

enum return_codes
{
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};

struct command_t
{
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3];		// in/out redirection
	struct command_t *next; // for piping
};

// cdh global variables
const int SIZE = 32;
const char *name = "OS";
char history[MAX_HISTROY_SIZE + 1][256];
char history_path[256];
int saveDir;

// cdh helper functions
int save_history();
int read_history_file();
int write_history_file();
int print_history();
void initialize_history_path();

// automata helper function
int set_random_automata(char **,int*);

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command)
{
	int i = 0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background ? "yes" : "no");
	printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
	printf("\tRedirects:\n");
	for (i = 0; i < 3; i++)
		printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
	printf("\tArguments (%d):\n", command->arg_count);
	for (i = 0; i < command->arg_count; ++i)
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
	if (command->arg_count)
	{
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i = 0; i < 3; ++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
		free_command(command->next);
		command->next = NULL;
	}
	free(command->name);
	free(command);
	return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
	char cwd[1024], hostname[1024];
	gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
	const char *splitters = " \t"; // split at whitespace
	int index, len;
	len = strlen(buf);
	while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
	{
		buf++;
		len--;
	}
	while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
		buf[--len] = 0; // trim right whitespace

	if (len > 0 && buf[len - 1] == '?') // auto-complete
		command->auto_complete = true;
	if (len > 0 && buf[len - 1] == '&') // background
		command->background = true;

	char *pch = strtok(buf, splitters);
	command->name = (char *)malloc(strlen(pch) + 1);
	if (pch == NULL)
		command->name[0] = 0;
	else
		strcpy(command->name, pch);

	command->args = (char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], *arg;

	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch)
			break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		if (len == 0)
			continue;										 // empty arg, go for next
		while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL)
			arg[--len] = 0; // trim right whitespace
		if (len == 0)
			continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|") == 0)
		{
			struct command_t *c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t')
				index++; // skip whitespaces

			parse_command(pch + index, c);
			pch[l] = 0; // put back strtok termination
			command->next = c;
			continue;
		}

		// background process
		if (strcmp(arg, "&") == 0)
			continue; // handled before

		// handle input redirection
		redirect_index = -1;
		if (arg[0] == '<')
			redirect_index = 0;
		if (arg[0] == '>')
		{
			if (len > 1 && arg[1] == '>')
			{
				redirect_index = 2;
				arg++;
				len--;
			}
			else
				redirect_index = 1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"') || (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}
		command->args = (char **)realloc(command->args, sizeof(char *) * (arg_index + 1));
		command->args[arg_index] = (char *)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;
	return 0;
}

void prompt_backspace()
{
	putchar(8);	  // go back 1
	putchar(' '); // write empty over
	putchar(8);	  // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
	int index = 0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

	// tcgetattr gets the parameters of the current terminal
	// STDIN_FILENO will tell tcgetattr that it should write the settings
	// of stdin to oldt
	static struct termios backup_termios, new_termios;
	tcgetattr(STDIN_FILENO, &backup_termios);
	new_termios = backup_termios;
	// ICANON normally takes care that one line at a time will be processed
	// that means it will return if it sees a "\n" or an EOF or an EOL
	new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
	// Those new settings will be set to STDIN
	// TCSANOW tells tcsetattr to change attributes immediately.
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

	// FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state = 0;
	buf[0] = 0;

	while (1)
	{
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c == 9) // handle tab
		{
			buf[index++] = '?'; // autocomplete
			break;
		}

		if (c == 127) // handle backspace
		{
			if (index > 0)
			{
				prompt_backspace();
				index--;
			}
			continue;
		}
		if (c == 27 && multicode_state == 0) // handle multi-code keys
		{
			multicode_state = 1;
			continue;
		}
		if (c == 91 && multicode_state == 1)
		{
			multicode_state = 2;
			continue;
		}
		if (c == 65 && multicode_state == 2) // up arrow
		{
			int i;
			while (index > 0)
			{
				prompt_backspace();
				index--;
			}
			for (i = 0; oldbuf[i]; ++i)
			{
				putchar(oldbuf[i]);
				buf[i] = oldbuf[i];
			}
			index = i;
			continue;
		}
		else
			multicode_state = 0;

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1)
			break;
		if (c == '\n') // enter key
			break;
		if (c == 4) // Ctrl+D
			return EXIT;
	}
	if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
		index--;
	buf[index++] = 0; // null terminate string

	strcpy(oldbuf, buf);

	parse_command(buf, command);

	//print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}

int process_command(struct command_t *command);

void filesearch(char* argument1,char* argument2,char* path){
	DIR *d;
	DIR *d2;
	struct dirent *dir;
	d=opendir(path);
	if(strcmp(path,".") == 0 || strcmp(path,"..")==0){}
	else{
		if(strcmp(argument1,"-o") == 0 ){
			//open files
			while((dir=readdir(d)) != NULL){
				if(strstr(dir->d_name,argument2)!= NULL){
					char* temp[]={"xdg-open",dir->d_name,NULL};
					execvp("xdg-open",temp);
					printf("\n");
				}
			}
		}
		if(strcmp(argument1,"-r") == 0){
			//recursive search
			while((dir=readdir(d)) != NULL){
				if(strcmp(dir->d_name,".")== 0 || strcmp(dir->d_name,"..")== 0 ||strcmp(dir->d_name,".git")== 0 ){}
				else{
					if(strstr(dir->d_name,argument2)!= NULL){
						printf("%s/%s \n",path,dir->d_name);
					}
					if((d2=opendir(dir->d_name)) != NULL){
						filesearch(argument1,argument2,dir->d_name);
					}
					closedir(d2);
				}	
			}
			closedir(d);
		}
		if(argument2== NULL){
			while((dir=readdir(d)) != NULL){
				if(strstr(dir->d_name,argument1)!= NULL){
					printf("%s \n",dir->d_name);
				}
			}
			closedir(d);
		}
	}
}
void take(char* arguments){
	char *c=strtok(arguments,"/");
	while(c != NULL){
		mkdir(c,0755);
		chdir(c);
		c=strtok(NULL,"/");
	}
}
void factors(int number){
	int i=2,oldi=i;
	printf("factors of %d are: \n",number);
	while(i<=number){
		if(number % i==0){
			printf(" %d",i);
			number=number/i;
			i=oldi;
		}else{
			oldi=i;
			i++;
		}
	}
	printf("\n");
	
}
int main()
{
	// Gets the HOME path to save cdh_history.txt file
	initialize_history_path();
	while (1)
	{
		struct command_t *command = malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0

		int code;
		code = prompt(command);
		if (code == EXIT)
			break;

		code = process_command(command);
		if (code == EXIT)
			break;

		free_command(command);
	}

	printf("\n");
	return 0;
}

int process_command(struct command_t *command)
{
	// Save history for cdh command
	save_history();

	int r;
	if (strcmp(command->name, "") == 0)
		return SUCCESS;

	if (strcmp(command->name, "exit") == 0)
		return EXIT;
	
	if (strcmp(command->name, "cd") == 0)
	{
		if (command->arg_count > 0)
		{
			r = chdir(command->args[0]);
			if (r == -1)
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));	
			return SUCCESS;
		
		}
	}

	// TODO: Implement your custom commands here
	if(strcmp(command->name,"filesearch") == 0 && (command->arg_count == 2 ||command->arg_count == 1)){
		filesearch(command->args[0],command->args[1],"./");
	}
	if(strcmp(command->name,"take") == 0 && command->arg_count == 1){
		take(command->args[0]);
	}
	if(strcmp(command->name,"factors")== 0 && command->arg_count == 1){
		factors(atoi(command->args[0]));
	}

	// joker command
	if (strcmp(command->name, "joker") == 0)
	{
		char *getJoke = malloc(sizeof(char) * 512);
		char *command = malloc(sizeof(char)* 2048); 

		// String gets the joke from link	
		strcpy(getJoke,"\"a dad:\" \"$(curl -s https://icanhazdadjoke.com/)\"");
		
		// Complete command
		strcpy(command,"crontab -l | { cat;echo \'15 * * * * XDG_RUNTIME_DIR=/run/user/$(id -u) notify-send ");
		strcat(command,getJoke);
		strcat(command," \'; } | crontab -");
		
		system(command);

		free(getJoke);
		free(command);
	
		return SUCCESS;
	}

	// automata command
	if (strcmp(command->name, "automata") == 0)
	{
		char **text = malloc(sizeof(char*) * 200);
		char temp[200];
		int totalLine = 0;
		set_random_automata(text,&totalLine);
		
		// Print
		int pageStart = 0;
		int line1Back = 0;
		int isBtwait = 0;
		int btWaitSkip = 0;
		
		for (int i = 0; i< totalLine -1;i++){
			
			int len = strlen(text[i]);
			isBtwait = 0;
			btWaitSkip = 0;
			
			//new page
			if (strncmp(text[i], "<page>",6) == 0){
				fputs("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n",stdout);
				pageStart = i;
				continue;	
			}
			// End page
			if (strncmp(text[i], "</page>",7) == 0){
				pageStart = i;
				continue;	
			}

			// Waiting
			char *wait = strstr(text[i],"<bt_wait>");
			int index = -1;
			if (wait != NULL){
				index = wait - text[i];
				
			}
			if (strncmp(text[i]+len - 11,"<bt_wait>",8) == 0){
				usleep(200000);
				isBtwait = 1;
				btWaitSkip = len - 11;
			}
			usleep(200000); // new line

			line1Back = i - 1;
			for(int j = 1; j < len; j++){
				
				// btwaits
				if (index != -1 && j == index + 11){
					sleep(1); 
				}
				
				// Animation //
				fputs("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n",stdout);
				
				// Prints text[pageStart,...,line1Back]
				for(int line = pageStart; line <= line1Back; line++){
					
					if (strncmp(text[line]+strlen(text[line]) - 11,"<bt_wait>",8) == 0){
						char tempWait[200];
						strcpy(tempWait,text[line]);
						tempWait[strlen(text[line]) - 11] = '\0';
						fputs(tempWait,stdout);
						fputs("\n",stdout);
						
					}else{
						if (strncmp(text[line], "<page>",6) == 0){
							continue;	
						}
						if (strncmp(text[line], "</page>",6) == 0){
							continue;
						}
						fputs(text[line],stdout);
					}

				}
				
					// Prints char by char
					strcpy(temp,text[i]);
					temp[j-1] = '\n'; 
					temp[j] = '\0';
					fputs(temp,stdout);
					usleep(60000);
						
			}
			
			
		}
		printf("\n");

		free(text); 
		
	}

	pid_t pid = fork();
	
	if (pid == 0) // child
	{	
		int shm_fd;
		void *ptr;

		// create the shared memory segment 
		shm_fd = shm_open(name, O_CREAT | O_RDWR, 0666);

		// configure the size of the shared memory segment 
		ftruncate(shm_fd,SIZE);

		// now map the shared memory segment in the address space of the process 
		ptr = mmap(0,SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
		if (ptr == MAP_FAILED) {
			printf("Map failed\n");
			return -1;
		}

		// Checks if cdh is called
		sprintf(ptr,"%d",-1);

		/* cdh command */
		if (strcmp(command->name, "cdh") == 0)
		{	
			// No arg cdh 
			if (command->arg_count == 0){
				char dirLetter;
				int letterIndex;
				print_history(saveDir);
				printf("Select directory by letter: ");
				scanf("%c",&dirLetter);

				letterIndex = dirLetter - 'a';

				// Writes the index of wanted dir to shared memory
				sprintf(ptr,"%d",letterIndex);
				
				exit(0);

			}else{

				// Removes history file with first argument 'remove'
				if (strcmp(command->args[0],"-r") == 0){
					remove(history_path);
					printf("cdh: History file removed\n");
					exit(0);
			}

			}
		
		}
		
		// increase args size by 2
		command->args = (char **)realloc(
			command->args, sizeof(char *) * (command->arg_count += 2));

		// shift everything forward by 1
		for (int i = command->arg_count - 2; i > 0; --i)
			command->args[i] = command->args[i - 1];

		// set args[0] as a copy of name
		command->args[0] = strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count - 1] = NULL;

		/// TODO: do your own exec with path resolving using execv()
    	char path[MAX_SIZE] = "/bin/";	
		strcat(path,command->name);
		
    	if (command->arg_count <= 1){ printf("Invalid input!\n "); return 0; }
		
      	execv(path, command->args);
      	exit(0);

	}
	else // Parent 
	{
		/// TODO: Wait for child to finish if command is not running in background
		
		if (!command->background){ 
			wait(NULL);
			int read_cdh;
		
			int shm_fd;
			void *ptr;
		
			// open the shared memory segment 
			shm_fd = shm_open(name, O_RDONLY, 0666);
			if (shm_fd == -1) {
				printf("shared memory failed\n");
				exit(-1);
			}

			// now map the shared memory segment in the address space of the process 
			ptr = mmap(0,SIZE, PROT_READ, MAP_SHARED, shm_fd, 0);
			if (ptr == MAP_FAILED) {
				printf("Map failed\n");
				exit(-1);
			}
			read_cdh = atoi(ptr);

			// Remove shared memory
			if (shm_unlink(name) == -1) {
			printf("Error removing %s\n",name);
			exit(-1);
	}

			// Go to wanted dir by cd command
			if (read_cdh != -1){
				int letterIndex = read_cdh;

				char goPath[200];
				strcpy(goPath,history[letterIndex]);
				
				if (saveDir -1 !=letterIndex)
					goPath[strlen(goPath) - 1] = '\0';

				// cd command 
				r = chdir(goPath);
				if (r == -1)
					printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));	
			}
		}
		
		return SUCCESS;
	}

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}


/* Helper Functions */

/**
 * Main 'cdh' command function which reads, modifies and writes
 * the recent directories
 * @return          [success]
 */
int save_history(){
	saveDir = 0;

	char currentDic[PATH_MAX];
	getcwd(currentDic, sizeof(currentDic));
	 
	// Read history file and get # of saved dirs 
	if (!read_history_file()){
		printf("cdh: History file created\n");
	}
	
	// Add current dic to history variable 
	if (saveDir >= MAX_HISTROY_SIZE ){
		// History is full
		for(int i = 0; i < saveDir - 1; i++){
			strcpy(history[i],history[i + 1]);
		}
		strcpy(history[saveDir-1],currentDic);
		
	}else{
		// History is not full
	  	strcpy(history[saveDir],currentDic);
		saveDir++;
	}
	
	if (!write_history_file()){
		printf("Failed to write History file\n");
		return 0;
	}
	
	return 0;
	
}

/**
 * Reads from HOME/cdh_history.txt and save it to history variable
 * @return            [success]
 */
int read_history_file(){
		
	FILE *file_read = fopen(history_path,"r");
	char line[PATH_MAX];

	if(file_read == NULL) { 
		// creates history file
		FILE *file_write = fopen(history_path,"w");
		fclose(file_write);
		return 0; 
	}	 
  
	while(1)
   	{
		fgets(line,PATH_MAX,file_read);
		if(feof(file_read)) break; 	
		// Save to variable history from file and get # el
	  	strcpy(history[saveDir],line);
	  	saveDir++;	
   	}
	fclose(file_read);
	
	return 1;
}
/**
 * Writes to HOME/cdh_history.txt from history variable
 * @return            [success]
 */
int write_history_file(){
	
	FILE *file_write = fopen(history_path,"w");

	if(file_write == NULL) { return 0; }	 
  
	for(int i = 0; i < saveDir -1; i++)
   	{
		fprintf(file_write, "%s", history[i]);
   	}
	fprintf(file_write, "%s\n", history[saveDir - 1]);
	fclose(file_write);
	
	return 1;
}
/**
 * Prints the history 
 * @return            [success]
 */
int print_history(){
	
	char charNumber = 'a';
	
	for(int i = 0; i < saveDir; i++){
		printf("%c)  %s",charNumber+i,history[i]);
	}
	printf("\n");
	return 1;

}

/**
 * Finds and saves the HOME path
 */
void initialize_history_path(){
	strcpy(history_path,getenv("HOME"));
	strcat(history_path,"/cdh_history.txt");
}

/**
 * Selects random automata file from the dir and reads it 
 * to automata variable
 */
int set_random_automata(char **automata, int *totalLine){

	int r_int = rand() % FILE_NUMBER;  
	char r_str[3];
	sprintf(r_str, "%d", r_int);

	char *path = malloc(sizeof(char*) * 100);
	strcpy(path,"automata/automata_");
	strcat(path,r_str);
	strcat(path,".txt");
	
	FILE *file_read = fopen(path,"r");
	char *line = (char*)malloc(200 * sizeof(char));  

	if(file_read == NULL) { 
		printf("Failed to read file\n");
		return 0; 
	}	 
  	*totalLine = 0;
	while(1)
   	{	
		fgets(line,1000,file_read);
		automata[*totalLine] = malloc(sizeof(char) * 200);
	  	strcpy(automata[*totalLine],line);
		(*totalLine)++;
		if(feof(file_read)) break; 
   	}

	fclose(file_read);
	free(line);

	return 1;

}