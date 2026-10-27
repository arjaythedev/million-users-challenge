So I was walking my dog and thinking about life when I suddenly
remembered an old video about computing Pi in CMake 
Then I remembered my skill issue drogon submission
And an idea was born inside my head....

It was obvious that you cant make a server in CMake alone, but I remembered about
the existence of add_custom_command(), so you can start processes from cmake.
After some digging (well of course I had to do that, 
it's not like I am writing apps in CMake every day), I found out about execute_process(), 
which allows executing processes (who would've imaginned...) during cmake runtime.

At that time I came up with some kind of project structure, but I wanted to throw something
really stupid into the mix. Then I remembered  writing a card game in Flat Assembly... 
and this is how launcher.asm was born (what, you think I would write a server in it? 
it's too hard even with vibecoding, especially for browser Deepseek. maybe I'll do it someday, 
but now I am a busy student, unfortunately)

Then I came home, grabbed my trusty 6-core friend and the rest is history.

### Language, framework, driver and versions

CMake latest, IdkWhatToWriteHereNoFrameworkMaybe, libpq, c++, NASM

### The optimizations you made and why

- Built CMake from source. We want PERFORMANCE!!!, after all
- Spared CMake the trouble of compiling cpp code. It is beneath the dignity of such a magnificent language  
  to concern itself with such sordid matters.
- Determined my brain and lazyness to be a bottleneck in development pipeline.  
So, major work was done by browser Deepseek, gotta hand it to him, he did really well, there is no way 
I would try doing this myself.

That is probably all. I've done enough, I'm satisfied. Gotta go to bed to wake up fresh, see my failure at  multicore-server-challenge, 
attend a job interview tomorrow and start developing a cpp + trantor + boost::asio submission for this challenge.

Wish everyone good luck!