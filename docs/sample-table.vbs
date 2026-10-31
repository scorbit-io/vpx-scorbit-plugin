' Scorbit in an original table's script: a sample to copy from.
'
' It identifies the table to Scorbit, reports the game, exposes the gameplay settings as VPX
' Table Options, and reads Scorbit's state back from a timer. Every use of Scorbit is guarded,
' so the table still runs on a VPX without the plugin. See "Table scripts" in the README.
'
' The table needs:
'   - the table object named Table1 (rename the event subs if yours differs);
'   - a Timer named ScorbitTimer, disabled in the editor (the script sets its interval and starts it);
'   - its own game logic calling StartGame, AddScore, NextBall and EndGame below.
' Replace ScorbitMachine with the uuid of your table's Scorbit machine.

Option Explicit

Const ScorbitMachine = "00000000-0000-0000-0000-000000000000"
Const MaxPlayers = 4

Dim Scorbit
On Error Resume Next
Set Scorbit = CreateObject("Scorbit.Table")
On Error Goto 0
If IsObject(Scorbit) Then Scorbit.Declare ScorbitMachine

' The table's own game state, as its game logic would keep it.
Dim PlayersInGame, CurrentPlayer, CurrentBall, GameInProgress
Dim Score(4)            ' 1 to MaxPlayers; 0 unused
Dim PlayerLabel(4)      ' what the table shows for each player: a Scorbit name once claimed
Dim LeaderboardText     ' for the table's attract mode
PlayersInGame = 1 : CurrentPlayer = 1 : CurrentBall = 1 : GameInProgress = False
LeaderboardText = ""

' --- Gameplay settings, as Table Options ---------------------------------------------------
' Settings that change how a score is made belong in Table Options, so every player can see
' them in VPX's options menu and the table's settings file records them. A changed setting
' takes effect at the next game start, never during a game.

Dim BallsPerGame, ExtraBalls                  ' in force for the current game; the game logic must honour both
Dim OptionBallsPerGame, OptionExtraBalls      ' as the options menu now has them
OptionBallsPerGame = 3 : OptionExtraBalls = 1
BallsPerGame = 3 : ExtraBalls = 1

' VPX calls this after Table1_Init (eventId 0), when an option changes (1), and when the
' options menu closes (3). Reading an option registers it on first use and returns its value.
' Keep the names stable: VPX stores each value under its option's name.
Sub Table1_OptionEvent(ByVal eventId)
   OptionBallsPerGame = Table1.Option("Balls per game", 3, 5, 1, 3, 0, Array("3", "4", "5"))
   OptionExtraBalls = Table1.Option("Extra balls", 0, 1, 1, 1, 0, Array("Off", "On"))
End Sub

' --- Start-up ----------------------------------------------------------------------------

Sub Table1_Init()
   ScorbitTimer.Interval = 500
   ScorbitTimer.Enabled = True
End Sub

' --- Reporting the game --------------------------------------------------------------------
' Calls are held by the plugin until Commit. ScorbitTimer commits twice a second, so the game
' logic only marks that something changed.

Dim ScorbitPending : ScorbitPending = False

Sub StartGame(players)
   Dim i
   If players > MaxPlayers Then players = MaxPlayers
   BallsPerGame = OptionBallsPerGame
   ExtraBalls = OptionExtraBalls
   PlayersInGame = players : CurrentPlayer = 1 : CurrentBall = 1 : GameInProgress = True
   For i = 1 To MaxPlayers
      Score(i) = 0
      PlayerLabel(i) = "PLAYER " & i
   Next
   If IsObject(Scorbit) Then
      Scorbit.SetGameStarted 0              ' 0 start button, 1 lobby
      Scorbit.SetActivePlayer 1
      Scorbit.SetCurrentBall 1
      ScorbitPending = True
   End If
End Sub

Sub AddScore(points)
   If Not GameInProgress Then Exit Sub
   Score(CurrentPlayer) = Score(CurrentPlayer) + points
   If IsObject(Scorbit) Then
      Scorbit.SetScore CurrentPlayer, Score(CurrentPlayer)
      ScorbitPending = True
   End If
End Sub

' Called when a mode starts or completes, e.g. ScorbitMode "MB:Multiball", False
Sub ScorbitMode(mode, completed)
   If IsObject(Scorbit) Then
      If completed Then Scorbit.SetModeCompleted mode Else Scorbit.AddMode mode
      ScorbitPending = True
   End If
End Sub

Sub NextBall()
   CurrentPlayer = CurrentPlayer + 1
   If CurrentPlayer > PlayersInGame Then
      CurrentPlayer = 1
      CurrentBall = CurrentBall + 1
   End If
   If CurrentBall > BallsPerGame Then
      EndGame
      Exit Sub
   End If
   If IsObject(Scorbit) Then
      Scorbit.SetCurrentBall CurrentBall
      Scorbit.SetActivePlayer CurrentPlayer
      ScorbitPending = True
   End If
End Sub

Sub EndGame()
   GameInProgress = False
   If IsObject(Scorbit) Then
      Scorbit.SetGameFinished                ' commits too
      ScorbitPending = False
   End If
   LeaderboardTicks = 20                     ' refresh the leaderboard in 10 s, with this game in it
End Sub

' --- Reading back ----------------------------------------------------------------------------
' ApiLevel 2 and later can read state back. An older plugin's object has no ApiLevel at all, so
' reading it there is an error: ScorbitApiLevel reads it guarded and reports 1 instead.

Function ScorbitApiLevel()
   ScorbitApiLevel = 0
   If Not IsObject(Scorbit) Then Exit Function
   On Error Resume Next
   ScorbitApiLevel = Scorbit.ApiLevel
   If Err.Number <> 0 Then ScorbitApiLevel = 1
   On Error Goto 0
End Function

Dim LeaderboardId : LeaderboardId = 0
Dim LeaderboardTicks : LeaderboardTicks = 0   ' ticks until the next request; while waiting, ticks since this one

Sub ScorbitTimer_Timer()
   If Not IsObject(Scorbit) Then Exit Sub
   If ScorbitPending Then
      Scorbit.Commit
      ScorbitPending = False
   End If
   If ScorbitApiLevel() < 2 Then Exit Sub

   ' Show a claimed player's Scorbit name in place of "PLAYER n".
   Dim slot
   If GameInProgress Then
      For slot = 1 To PlayersInGame
         If Scorbit.PlayerClaimed(slot) Then PlayerLabel(slot) = Scorbit.PlayerName(slot) Else PlayerLabel(slot) = "PLAYER " & slot
      Next
   End If

   ' Between games, keep this machine's top 5 for the attract mode, asked for every 5 minutes.
   If GameInProgress Then Exit Sub
   If LeaderboardId = 0 Then
      LeaderboardTicks = LeaderboardTicks - 1
      If LeaderboardTicks <= 0 Then
         ' scope 0 this machine, period 0 all time, vpinFilter 0 any, limit 5, since ""
         LeaderboardId = Scorbit.RequestTopScores(0, 0, 0, 5, "")
         ' 0: no daemon can answer yet (it takes a few seconds to connect), so try again in 5 s.
         If LeaderboardId = 0 Then LeaderboardTicks = 10 Else LeaderboardTicks = 0
      End If
   ElseIf Scorbit.TopScoresResult(LeaderboardId) < 0 Then
      ' Still -1 after a minute: the answer is lost (or the id was dropped), so ask again.
      LeaderboardTicks = LeaderboardTicks + 1
      If LeaderboardTicks > 120 Then
         Scorbit.ReleaseTopScores LeaderboardId
         LeaderboardId = 0
         LeaderboardTicks = 0
      End If
   Else
      If Scorbit.TopScoresResult(LeaderboardId) = 0 Then
         Dim i, text
         text = ""
         For i = 0 To Scorbit.TopScoresCount(LeaderboardId) - 1
            text = text & Scorbit.TopScoresRank(LeaderboardId, i) & ". " & Scorbit.TopScoresName(LeaderboardId, i) _
               & "  " & FormatNumber(Scorbit.TopScoresScore(LeaderboardId, i), 0) & vbCrLf
         Next
         LeaderboardText = text
      End If
      Scorbit.ReleaseTopScores LeaderboardId
      LeaderboardId = 0
      LeaderboardTicks = 600                 ' the next one in 5 minutes
   End If
End Sub
